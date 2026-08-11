#define DLLEXPORT __declspec(dllexport)

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <chrono>
#include <shlobj.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>
#include <thread>

#include "DelayManager.hpp"
#include "EditorIDLoader.hpp"
#include "IniParser.hpp"
#include "MNAMResolver.hpp"
#include "NPCManager.hpp"
#include "NPCModifier.hpp"
#include "NPCSwapper.hpp"
#include "Utils.hpp"
#include "serialization.hpp"

#include <memory>

namespace
{

// Hydra loads every EditorID we may reference. Without it we fall back to
// EditorIDLoader (Baka-derived) which misses a few form types. Prompt and
// let the user decide on Cancel/Continue.
bool CheckHydraDependency()
{
    if (GetModuleHandleA("Hydra.dll") != nullptr) return true;

    const int result = MessageBoxA(nullptr,
                                   "Base NPC Swapper requires Hydra to function fully (for EditorIDs).\n\n"
                                   "Hydra loads form EditorIDs that Fallout 4 does not load by default. "
                                   "Without Hydra, BNS rules that reference a form of type NPC_ by EditorID "
                                   "will fail to resolve.\n\n"
                                   "Download Hydra:\n"
                                   "https://www.nexusmods.com/fallout4/mods/104159\n\n"
                                   "OK     - Launch anyway. Only rules without NPC_ in editorIDs will work.\n"
                                   "Cancel - Exit the game now so you can install Hydra (recommended)",
                                   "Base NPC Swapper - Missing Dependency: Hydra",
                                   MB_OKCANCEL | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);

    spdlog::error("Hydra.dll not detected. User chose: {}", (result == IDOK) ? "CONTINUE" : "EXIT");

    if (result != IDOK)
    {
        ExitProcess(1); // Terminate the game cleanly.
    }
    return false; // Load what we can
}

using _Load3D = RE::NiAVObject* (*)(RE::Actor*, bool);
_Load3D Original_Actor_Load3D = nullptr;

RE::NiAVObject* Hooked_Actor_Load3D(RE::Actor* a_this, bool a_backgroundLoading)
{
    RE::NiAVObject* result = Original_Actor_Load3D ? Original_Actor_Load3D(a_this, a_backgroundLoading) : nullptr;

    auto* manager = BNS::NPCManager::GetSingleton();
    if (manager->IsShuttingDown() || !a_this) return result;

    // Never route the player through the pipeline. A misconfigured rule that
    // matched would SetObjectReference on 0x14 — guaranteed save corruption.
    if (a_this->GetFormID() == 0x14) return result;

    manager->QueueDeferredEvaluation(a_this->GetFormID());
    return result;
}

void InstallHooks()
{
    // One-shot. F4SE shouldn't fire kGameDataReady twice, but if it ever
    // does, a second install would chain our hook onto our own previous
    // hook and every actor would be queued twice.
    static bool installed = false;
    if (installed) return;
    installed = true;

    REL::Relocation<std::uintptr_t> actorVTable {RE::VTABLE::Actor[0]};

    Original_Actor_Load3D = reinterpret_cast<_Load3D>(actorVTable.write_vfunc(0x86, Hooked_Actor_Load3D));

    spdlog::info("---------------------------------------------------------------");
    spdlog::info("Successfully installed Actor::Load3D VTable Hook at index 0x86.");
    spdlog::info("---------------------------------------------------------------");
}

bool UninstallBNS(std::monostate)
{
    auto* manager = BNS::NPCManager::GetSingleton();
    if (manager->IsShuttingDown()) return false;

    manager->RequestShutdown();
    spdlog::info("Shutdown requested. Hook sealed. Waiting for active swaps to finish...");

    std::thread([manager]() {
        // Hard cap so a Papyrus VM hang can't freeze the MCM uninstaller flow
        // forever. The pipeline is drained on a best-effort basis; the user
        // can always exit-and-restart if something is truly stuck.
        constexpr auto kShutdownWaitCap = std::chrono::seconds(30);
        const auto deadline = std::chrono::steady_clock::now() + kShutdownWaitCap;
        while (!manager->IsPipelineEmpty())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                spdlog::warn("Shutdown wait hit 30s cap with pipeline still busy — proceeding with uninstall anyway.");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (manager->IsPipelineEmpty()) spdlog::info("Shutdown complete. Pipeline is empty. Safe to uninstall.");
        BNS::Utils::DispatchToPapyrus("BNSUninstaller", "OnUninstallComplete");
    }).detach();

    return true;
}

// Forwarded from BNSSpawnHelper.ResetActorState's tail call. Unblocks the
// Phase-8 resume queued in NPCSwapper for this refID. Fires exactly once
// per pending entry (race with the timeout is consumed atomically inside).
bool BNSResetActorStateDone(std::monostate, std::int32_t a_refActor)
{
    BNS::Swapper::OnResetActorStateDone(static_cast<std::uint32_t>(a_refActor));
    return true;
}

// AttachModsToItem -> here. Queues a re-equip for after the drain if the
// item was originally equipped and the attach actually happened.
bool BNSOnAttachComplete(std::monostate, std::int32_t a_refActor, std::int32_t a_refItem, bool a_wasEquipped, bool a_attached)
{
    BNS::Modifier::OnAttachComplete(static_cast<std::uint32_t>(a_refActor),
                                    static_cast<std::uint32_t>(a_refItem),
                                    a_wasEquipped,
                                    a_attached);
    return true;
}

// EquipItemSafe -> here. Decrements the per-actor in-flight equip batch;
// when it reaches zero the modification pipeline advances to the next rule.
bool BNSOnEquipComplete(std::monostate, std::int32_t a_refActor, std::int32_t a_refItem, bool a_equipped)
{
    BNS::Modifier::OnEquipComplete(static_cast<std::uint32_t>(a_refActor),
                                   static_cast<std::uint32_t>(a_refItem),
                                   a_equipped);
    return true;
}

// BNSSpawnHelper.PapyrusDisable / PapyrusEnable -> here. Wakes Phase 1
// (Disable) or Phase 4 (Enable) only after the VM has actually executed
// the call — under heavy facegen load (large face pools, e.g. Diverse
// Wasteland) the fire-and-forget version would let Phase 2 / Phase 5
// start before the engine finished processing the previous Disable /
// Enable, crashing the IOManager / BSJobs thread.
bool BNSOnPapyrusDisableDone(std::monostate, std::int32_t a_refActor)
{
    BNS::Swapper::OnPapyrusDisableDone(static_cast<std::uint32_t>(a_refActor));
    return true;
}

bool BNSOnPapyrusEnableDone(std::monostate, std::int32_t a_refActor)
{
    BNS::Swapper::OnPapyrusEnableDone(static_cast<std::uint32_t>(a_refActor));
    return true;
}

// Console diagnostic — reachable via `cgf "BNSSpawnHelper.DumpInfo" <refID>`.
// Hops to main so we read actor state from the engine's owning thread.
bool BNSDumpInfo(std::monostate, std::int32_t a_refActor)
{
    const auto refID = static_cast<std::uint32_t>(a_refActor);
    if (auto* task = F4SE::GetTaskInterface())
        task->AddTask([refID]() { BNS::Swapper::DumpActorInfo(refID); });
    else
        BNS::Swapper::DumpActorInfo(refID);
    return true;
}

// Refused during shutdown — clearing then would just create new in-progress
// work that the shutdown is trying to drain.
bool ClearBNSCache(std::monostate)
{
    auto* manager = BNS::NPCManager::GetSingleton();
    if (manager->IsShuttingDown())
    {
        spdlog::warn("ClearBNSCache called during shutdown - refused.");
        return false;
    }

    manager->ClearProcessedActors();
    return true;
}

// Forward-decl: definition lives below BuildPerRuleOMODCaches because it
// references that helper. Keep this in sync with the signature there.
bool BNSReloadRules(std::monostate);

bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* a_vm)
{
    a_vm->BindNativeMethod("BNSUninstaller", "UninstallBNS", UninstallBNS);
    a_vm->BindNativeMethod("BNSUninstaller", "ClearBNSCache", ClearBNSCache);
    a_vm->BindNativeMethod("BNSUninstaller", "BNSReloadRules", BNSReloadRules);
    a_vm->BindNativeMethod("BNSSpawnHelper", "BNSResetActorStateDone", BNSResetActorStateDone);
    a_vm->BindNativeMethod("BNSSpawnHelper", "BNSOnAttachComplete", BNSOnAttachComplete);
    a_vm->BindNativeMethod("BNSSpawnHelper", "BNSOnEquipComplete", BNSOnEquipComplete);
    a_vm->BindNativeMethod("BNSSpawnHelper", "BNSOnPapyrusDisableDone", BNSOnPapyrusDisableDone);
    a_vm->BindNativeMethod("BNSSpawnHelper", "BNSOnPapyrusEnableDone", BNSOnPapyrusEnableDone);
    a_vm->BindNativeMethod("BNSSpawnHelper", "BNSDumpInfo", BNSDumpInfo);
    spdlog::info("Papyrus functions registered.");
    return true;
}

// Runs on the main thread after MNAMResolver::BuildCache populates the
// shared MNAM map; mutates per-rule OMODCache while the pipeline is gated
// off by MNAMResolver::IsCacheReady() returning false.
void BuildPerRuleOMODCaches(std::vector<SwapRuleResolved>& a_rules)
{
    for (auto& rule : a_rules)
    {
        rule.OMODCache = BNS::Modifier::BuildOMODCache(rule.addOMODs);
    }
}

// Reload Rules flow. Triggered by MCM button → BNSUninstaller.StartReloadRules
// → BNSReloadRules native.
//
// Order of operations:
//   1. Gate the Load3D hook (BeginReload).
//   2. Drain in-flight pipelines on a worker thread, with a 30s cap.
//   3. Re-parse INIs + re-resolve rules off the main thread (no engine
//      mutation), incremental MNAM scan for any OMODs not already cached.
//   4. Hop to main thread for the per-rule OMOD cache rebuild + SetRules
//      + session-set wipe + EndReload.
//   5. Fire BNSUninstaller.OnReloadComplete via Papyrus.
//
// Refused while shutting down or already reloading.
bool BNSReloadRules(std::monostate)
{
    auto* manager = BNS::NPCManager::GetSingleton();
    if (manager->IsShuttingDown())
    {
        spdlog::warn("BNSReloadRules: refused — uninstall in progress.");
        return false;
    }
    if (manager->IsReloading())
    {
        spdlog::warn("BNSReloadRules: refused — another reload is already running.");
        return false;
    }

    manager->BeginReload();
    spdlog::info("[Reload] Requested. Pipeline gated; draining in-flight swaps...");

    std::thread([manager]() {
        constexpr auto kDrainCap = std::chrono::seconds(30);
        const auto deadline = std::chrono::steady_clock::now() + kDrainCap;
        while (!manager->IsPipelineEmpty())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                spdlog::warn("[Reload] Drain hit 30s cap with pipeline still busy — proceeding anyway.");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        // Off-thread: parse + resolve INIs, incremental MNAM scan. No engine
        // mutation here; reads file system only.
        std::filesystem::path iniDir = "Data\\F4SE\\Plugins\\BaseNPCSwapper";
        if (!std::filesystem::exists(iniDir))
        {
            spdlog::warn("[Reload] INI directory missing — keeping current rules.");
            manager->EndReload();
            BNS::Utils::DispatchToPapyrus("BNSUninstaller", "OnReloadComplete", static_cast<std::int32_t>(0));
            return;
        }
        auto rawRules = ParseIniFiles(iniDir);
        auto newRules = ResolveRules(rawRules);
        const auto ruleCount = static_cast<std::int32_t>(newRules.size());
        spdlog::info("[Reload] Resolved {} rules. Incremental MNAM scan for any new OMODs...", ruleCount);
        BNS::MNAMResolver::BuildCacheIncremental(newRules);

        // Hop to main thread for the atomic swap. Captures newRules by move
        // into the task-interface lambda — task->AddTask itself stores a
        // std::function which is copy-constructed; wrap the move in a
        // shared_ptr to dodge that.
        auto* task = F4SE::GetTaskInterface();
        if (!task)
        {
            spdlog::error("[Reload] TaskInterface unavailable post-MNAM — keeping old rules.");
            manager->EndReload();
            BNS::Utils::DispatchToPapyrus("BNSUninstaller", "OnReloadComplete", static_cast<std::int32_t>(0));
            return;
        }
        auto pendingRules = std::make_shared<std::vector<SwapRuleResolved>>(std::move(newRules));
        task->AddTask([manager, pendingRules, ruleCount]() {
            BuildPerRuleOMODCaches(*pendingRules);
            manager->SetRules(std::move(*pendingRules));
            manager->ClearSessionActorsOnly();
            manager->EndReload();
            spdlog::info("[Reload] Complete. {} rules active. Pipeline resumed.", ruleCount);
            BNS::Utils::DispatchToPapyrus("BNSUninstaller", "OnReloadComplete", ruleCount);
        });
    }).detach();

    return true;
}

void MessageCallback(F4SE::MessagingInterface::Message* a_msg)
{
    if (!a_msg) return;

    if (a_msg->type == F4SE::MessagingInterface::kPostLoad)
    {

        if (!CheckHydraDependency())
        {
            spdlog::error("ERROR: Hydra not loaded. Only rules without NPC_'s in editorIDs will work properly.");
            BNS::EditorIDLoader::InstallIfNeeded();
        }
        else
        {
            spdlog::info("Hydra found! EditorIDs will work fine.");
        }

        return;
    }

    if (a_msg->type == F4SE::MessagingInterface::kGameDataReady)
    {
        spdlog::info("Game data ready. Locating INI files...");
        if (!BNS::EditorIDLoader::IsWorking())
        {
            spdlog::error("EditorID lookup probe FAILED ('LCharDeathclaw' did not resolve). "
                          "Rules referencing forms by EditorID will not work. "
                          "Install Hydra (https://www.nexusmods.com/fallout4/mods/104159), or check the log "
                          "for [EditorIDLoader] errors during kPostLoad.");
        }
        else
        {
            spdlog::info("EditorID lookup probe SUCCESS!");
        }

        auto* manager = BNS::NPCManager::GetSingleton();
        manager->StartPipeline(); // starts the DelayManager worker thread

        std::filesystem::path iniDir = "Data\\F4SE\\Plugins\\BaseNPCSwapper";
        if (!std::filesystem::exists(iniDir)) std::filesystem::create_directories(iniDir);

        auto rawRules = ParseIniFiles(iniDir);
        auto resolvedRules = ResolveRules(rawRules);
        spdlog::info("Successfully resolved {} rules!", resolvedRules.size());

        // Hook + rules go live immediately. Actors loaded before the OMOD
        // cache finishes still get evaluated and swapped; only the OMOD
        // step in ApplyModificationsAsync gates on IsCacheReady() and
        // requeues itself.
        manager->SetRules(std::move(resolvedRules));
        InstallHooks();

        // MNAM parsing is the expensive piece (hundreds of MB of ESP I/O on
        // a large modlist); off-thread it so the main menu doesn't freeze.
        // Per-rule OMODCache mutation happens on the main thread to avoid
        // racing pipeline reads of m_rules.
        BNS::DelayManager::QueueTask(0, [manager]() {
            spdlog::info("[Startup] Scanning installed plugins for OMOD definitions "
                         "(this can take a few seconds on large modlists)...");
            BNS::MNAMResolver::BuildCache(manager->GetMutableRules());
            spdlog::info("[Startup] OMOD plugin scan finished. Indexing OMODs against each rule...");

            auto* task = F4SE::GetTaskInterface();
            if (!task)
            {
                // No TaskInterface — mark ready with empty per-rule caches
                // so the requeue loop doesn't spin forever. OMOD picks
                // will just be empty.
                spdlog::error("[Startup] F4SE task interface unavailable — cannot finish OMOD setup. "
                              "Rules that add OMODs will not attach anything this session.");
                BNS::MNAMResolver::MarkCacheReady();
                return;
            }
            task->AddTask([manager]() {
                BuildPerRuleOMODCaches(manager->GetMutableRules());
                BNS::MNAMResolver::MarkCacheReady();
                spdlog::info("[Startup] OMOD setup complete. Rules that add OMODs are now active.");
            });
        });
    }
}

} // namespace

void InitializeLog()
{
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, 0, path)))
    {
        std::string logPath = std::string(path) + "\\My Games\\Fallout4\\F4SE\\BaseNPCSwapper.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
        log->set_level(spdlog::level::debug);
        log->flush_on(spdlog::level::debug);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }
}

// Plugin metadata seen by F4SE before Load. CompatibleVersions lists every
// NG/AE runtime libxse knows about so the same DLL is accepted across the
// full NG/AE range — users install the Address Library matching their exe.
// IsLayoutDependent(true) is intentional: we read engine struct offsets
// (e.g. Actor::race at 0x418, vtable index 0x86), so F4SE should refuse to
// load us on any runtime not in this list rather than risk a crash.
// Pre-NG 1.10.163 ("Original Gen") has a different layout and is NOT
// supported by this DLL — it needs a separate fork of CommonLibF4.
F4SE_PLUGIN_VERSION = []() noexcept {
	F4SE::PluginVersionData v{};
	v.PluginVersion({ 0, 9, 1, 0 });
	v.PluginName("BaseNPCSwapper");
	v.AuthorName("mraggi");
	v.UsesAddressLibrary(true);
	v.UsesSigScanning(false);
	v.HasNoStructUse(false);
	v.IsLayoutDependent(true);
	v.CompatibleVersions({
		F4SE::RUNTIME_1_10_984,
		F4SE::RUNTIME_1_11_137,
		F4SE::RUNTIME_1_11_159,
		F4SE::RUNTIME_1_11_169,
		F4SE::RUNTIME_1_11_191,
		F4SE::RUNTIME_1_11_221,
	});
	return v;
}();

extern "C" DLLEXPORT bool F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);
    InitializeLog();
    spdlog::info("BNSF4_Plugin loaded.");

    auto messaging = F4SE::GetMessagingInterface();
    messaging->RegisterListener(MessageCallback);

    auto papyrus = F4SE::GetPapyrusInterface();
    papyrus->Register(RegisterPapyrusFunctions);

    auto serialization = F4SE::GetSerializationInterface();
    serialization->SetUniqueID(BNS::Serialization::BNSF4_SERIALIZATION_ID);
    serialization->SetSaveCallback(BNS::Serialization::SaveCallback);
    serialization->SetLoadCallback(BNS::Serialization::LoadCallback);
    serialization->SetRevertCallback(BNS::Serialization::RevertCallback);
    spdlog::info("Serialization interfaces registered.");

    return true;
}
