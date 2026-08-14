#include "NPCSwapper.hpp"
#include "DelayManager.hpp"
#include "Utils.hpp"

#include <F4SE/F4SE.h>
#include <spdlog/spdlog.h>

#include <RE/Fallout.h>

#include <format>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// =============================================================================
// Swap pipeline — see CLAUDE.md ("Swap pipeline" and "Engine quirks worth
// remembering") for the failure-mode history that shaped this. Quick summary:
//
//   Step 1 → Step 2 → Step 3 → Step 4
//                                ├─ broken: Step4_RetryDisable → _RetryEnable
//                                │          ↺ back to Step 4
//                                └─ clean : Step 5 → Step 6 → Step 7 → Step 8
//                                                                       ├─ broken: ↺ Step 6
//                                                                       └─ clean : Step 9 → Step 10
//
//   1   Papyrus Disable                                         [TIMEOUT 20s]
//   2   RemoveExtra(kLeveledCreature) + SetObjectReference(target)
//       + EnsureRobotInstanceData      <-- robots/eyebots/sentries need this
//   3   Papyrus Enable + poll facegen-settle                    [TIMEOUT 20s]
//   4   IsResolutionBroken check  → retry if race == SynthGen2 fallback
//   5   Actor::InitDefaultWornImpl(true, true)  → adds + equips default outfit
//   6   Final Papyrus Disable          \  ONLY combination empirically
//   7   Final Papyrus Enable + poll    /  observed to clear T-poses
//       (EnsureRobotInstanceData call repeats — every Disable strips it.
//        Reset3D / ResetHavokPhysics / HandleDefaultAnimationSwitch were
//        tried as T-pose fixes and did nothing.)
//   8   IsResolutionBroken check  → retry if race == SynthGen2 fallback
//   9   Papyrus ResetActorState (HP/limbs only — has no Disable/Enable now,
//                                so it can't re-roll templates)
//   10  SetPosition/SetActorAngle + release face-gen lock + onComplete
//
// Each step function has a DumpWithLabel(...) commented out at entry — flip
// any of them on while investigating a regression to get a state snapshot.
//
// Old 9-phase pipeline preserved in OldCrap.hpp.
// =============================================================================

namespace BNS::Swapper
{

namespace
{
    // -------------------------------------------------------------------------
    // Tunable constants. Tags:
    //   [TESTABLE] We poll an actual engine flag instead of guessing a time.
    //   [GUESS]    Pure safety margin; usually fine to set lower.
    //   [TIMEOUT]  Worst-case fallback so the chain can't deadlock.
    //   [CAP]      Loop bound.
    // -------------------------------------------------------------------------

    // [GUESS] Inter-step pause when no async engine work is in flight (e.g.
    // after SetObjectReference). 0 also works theoretically; this is just a
    // paranoia floor.
    constexpr int kBetweenStepGuessMS = 32;

    // [GUESS] Settle wait before the FINAL Disable (Step 5 -> 6). Bigger than
    // kBetweenStepGuessMS because under heavy cell load the engine is still
    // draining the outfit/3D work kicked off by InitDefaultWornImpl; tearing
    // 3D down too early here is a suspected cause of post-swap T-poses.
    // Tune in-game.
    constexpr int kBeforeFinalDisableMS = 256;

    // [GUESS] Settle wait between the FINAL Disable and Enable (Step 6 -> 7).
    // Was effectively zero — Enable fired the instant the Disable VM callback
    // returned. Gives the engine time to finish tearing 3D down after Disable
    // before Enable rebuilds it. Tune in-game.
    constexpr int kBetweenFinalDisableEnableMS = 128;

    // [TESTABLE] Post-Enable settle poll. The engine kicks async 3D / facegen /
    // animation-graph loads on its IOManager + job threads when Enable returns.
    // We poll until all three are ready (see IsActorSettled): 3D present,
    // faceGenLoadPending clear, AND the animation graph bound — the graph being
    // the part that actually clears T-poses. Robots/creatures usually return on
    // the first probe; humans typically clear in 50–200ms; very deep facegen
    // pools (Diverse Wasteland) can need the full cap.
    constexpr int kFaceGenPollIntervalMS = 32; // ~2 frames at 60 fps
    constexpr int kFaceGenMaxAttempts = 128; // 128 × 32 = ~4.1s cap

    // [TIMEOUT] Worst-case wait for a Papyrus VM callback. The VM is
    // single-threaded and can backlog under heavy spawn waves. On hit we
    // log a warning and let the chain continue.
    constexpr int kPapyrusAwaitTimeoutMS = 1024 * 24;

    // [CAP] Maximum bare Disable→Enable retries when IsResolutionBroken
    // catches an LVLN-in-slot / partial-resolution / null state (the
    // "Legendary Cataphract" failure mode — engine bailed mid-resolution).
    // Each retry is a fresh roll, so probability compounds; 8 should cover almost all cases.
    constexpr int kMaxResolveRetries = 8;

    // Allow parallel swaps for different actors targeting the same base.
    // Per-base lock below is a relic of the OLD 9-phase pipeline (OldCrap.hpp),
    // which mutated the shared TESNPC gender flag mid-swap. Current pipeline
    // doesn't touch shared NPC state, so the lock is unnecessary.
    constexpr bool kAllowParallelSwaps = true;

    std::mutex g_faceGenMutex;
    std::unordered_set<std::uint32_t> g_activeFaceGenBases;
    std::unordered_map<std::uint32_t, std::vector<std::function<void()>>> g_pendingSwaps;

    Utils::AsyncTokenRegistry<std::uint32_t> g_disableRegistry;
    Utils::AsyncTokenRegistry<std::uint32_t> g_enableRegistry;
    Utils::AsyncTokenRegistry<std::uint32_t> g_resetRegistry;

    struct SwapContext
    {
        std::uint32_t formID = 0;
        std::uint32_t targetBaseID = 0;
        int debugLevel = 0;
        RE::TESBoundObject* targetBase = nullptr;
        std::string ruleTag; // "file.ini | RuleName" — log prefix
        RE::NiPoint3 startPos {};
        RE::NiPoint3 startRot {};
        // Independent caps for the two retry loops (Step 4 / Step 8).
        int resolveRetries = 0;
        int finalRetries = 0;
        // EnableStateParent (XESP) extra detached at kickoff and re-attached at
        // the terminal. An enable-parented ref ignores our Papyrus Disable
        // (no-op) AND its Enable skips template resolution → SynthGen2 floating
        // head. Detaching makes the whole cycle behave like a plain ref; we hold
        // the exact extra (parent ref + flags intact) and restore it in
        // FinishSwapChain. nullptr when the source had no enable parent.
        RE::BSExtraData* savedEnableParent = nullptr;
        std::function<void()> onComplete;
    };

    // -------------------------------------------------------------------------
    // Detectors & state-mutators
    // -------------------------------------------------------------------------

    // SynthGen2RaceValentine — the engine's "unresolved / unknown race" fallback
    // (last entry in the race enum). A successfully resolved actor NEVER ends up
    // with it; a bailed template resolution ALWAYS does (gen-2-synth head, no
    // body → the "floating face"). See docs/EngineQuirks.md.
    constexpr std::uint32_t kSynthGen2RaceValentine = 0x002261A4;

    // Broken-resolution detector. The one authoritative symptom of the bug we
    // care about (floating gen-2 head) is the resolved race being the SynthGen2
    // fallback — true for BOTH flavors of bail (no ExtraLeveledCreature at all,
    // and a degenerate LVLN/partial chain), since asking any unresolved chain
    // for its race returns this same fallback.
    //
    // This replaces the older 13-slot ExtraLeveledCreature template scan, which
    // false-positived on every healthy leaf that legitimately keeps a non-Traits
    // template bit (e.g. a molerat leaf with kFactions = 1<<2 set). That scan
    // flagged 11/11 *correctly-resolved* molerats as "broken" and burned the
    // full retry budget on each for nothing. Race is the field that actually
    // governs whether the actor renders. Returns false on no-actor noise.
    bool IsResolutionBroken(std::uint32_t a_refID, std::string& a_reasonOut)
    {
        auto* form = RE::TESForm::GetFormByID(a_refID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor) return false;

        if (actor->race && actor->race->GetFormID() == kSynthGen2RaceValentine)
        {
            a_reasonOut = "resolved race is SynthGen2RaceValentine "
                          "(engine bailed mid-resolution → floating gen-2 head)";
            return true;
        }
        return false;
    }

    //
    // ============================================================================
    // ⚠ FUTURE-AI NOTE — DO NOT REMOVE OR REORDER EnsureRobotInstanceData CALLS ⚠
    // ============================================================================
    //
    // ROBOTS in Fallout 4 (Gunner Warmaster, Eyebots, Mr. Handies, Sentry Bots,
    // etc.) do NOT use default outfits or container CNTO records for their
    // visible body. Their pieces come from a BGSMod::Template::Items recipe
    // attached to the base form, which the engine fn
    // BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra materializes
    // onto the actor's extraList. Without this, robots come back from a swap
    // fully invisible.
    //
    // CRITICAL fact discovered (and re-discovered) the hard way:
    //   Every Papyrus Disable strips this instance data.
    //   So EnsureRobotInstanceData must run between EVERY Disable and the
    //   Enable that follows it. The pipeline currently calls it at three
    //   sites — Step 2 (after the first Disable), Step 7 (after the final
    //   Disable), and Step4_RetryEnable (after a retry Disable). Removing
    //   or reordering any of those breaks robots. This has happened MULTIPLE
    //   times; please leave it alone or you will get to fix warmaster again.
    //
    // Canonical regression test: rule with replaceBy = Gunner Warmaster
    // [16033267] (RA_Gunners.esp). Visible after the swap = call working.
    //
    // No-op if anything is null.
    //
    void EnsureRobotInstanceData(const SwapContext& a_ctx)
    {
        auto* form = RE::TESForm::GetFormByID(a_ctx.formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor || !actor->extraList || !a_ctx.targetBase) return;
        RE::BGSMod::Template::Items::CreateInstanceDataForObjectAndExtra(*a_ctx.targetBase,
                                                                         *actor->extraList,
                                                                         nullptr,
                                                                         true);
    }

    // A source actor wearing power armor carries an ExtraPowerArmor marker on
    // its extraList. If that marker survives the SetObjectReference, the next
    // Enable rebuilds the NEW base as a power-armor occupant — wrapping it in a
    // frame (a fresh ArmorPoweredFrame gets equipped) and leaving the actor's
    // own body unrendered. For a robot target this presents as the warmaster
    // coming back fully invisible. Confirmed 2026-06-11: monster->warmaster and
    // non-PA-human->warmaster render fine; only PA-human->warmaster was broken,
    // and the lone state difference was a surviving kPowerArmor extra.
    //
    // This runs in Step 2 while the actor is disabled and BEFORE
    // SetObjectReference, so:
    //   (1) Removing the kPowerArmor markers makes the Enable build the actor as
    //       a normal robot (the visibility fix), and
    //   (2) the inventory at this point is still purely the SOURCE's, so the
    //       source's PA gear (frame + T-60/X-01/… pieces) can be removed cleanly
    //       — they'd otherwise linger as ghost loot on the new base. The new
    //       base's own gear is added afterwards by Enable, so a swap whose
    //       *target* legitimately wears PA is unaffected.
    //
    // PA pieces are identified by the engine's power-armor keyword. Stacks bound
    // to a quest alias (kFromAlias / kAliasInstanceArray) are never removed, so a
    // swap can't delete an item a quest is tracking. No-op when the source isn't
    // in power armor, so non-PA swaps are entirely unaffected.
    void StripPowerArmorState(const SwapContext& a_ctx)
    {
        auto* form = RE::TESForm::GetFormByID(a_ctx.formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor || !actor->extraList) return;
        if (!actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor)) return;

        actor->extraList->RemoveExtra(RE::EXTRA_DATA_TYPE::kPowerArmor);
        actor->extraList->RemoveExtra(RE::EXTRA_DATA_TYPE::kInactivePowerArmor);
        actor->extraList->RemoveExtra(RE::EXTRA_DATA_TYPE::kPowerArmorPreload);

        int removed = 0;
        auto* paKeyword = RE::PowerArmor::GetArmorKeyword();
        if (paKeyword && actor->inventoryList)
        {
            // Collect first, then remove — don't mutate the list mid-iteration.
            std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> toRemove;
            for (const auto& item : actor->inventoryList->data)
            {
                if (!item.object) continue;
                auto* kwForm = item.object->As<RE::BGSKeywordForm>();
                if (!kwForm || !kwForm->HasKeyword(paKeyword)) continue;

                std::int32_t total = 0;
                bool questBound = false;
                for (auto stack = item.stackData; stack; stack = stack->nextStack)
                {
                    total += static_cast<std::int32_t>(stack->count);
                    if (auto* ex = stack->extra.get())
                        questBound = questBound || ex->HasType(RE::EXTRA_DATA_TYPE::kFromAlias)
                          || ex->HasType(RE::EXTRA_DATA_TYPE::kAliasInstanceArray);
                }
                if (questBound)
                {
                    spdlog::info("[Swap {:08X}] [Step 2] Keeping quest-bound PA item '{}' [{:08X}].",
                                 a_ctx.formID,
                                 Utils::GetFormName(item.object),
                                 item.object->GetFormID());
                    continue;
                }
                if (total > 0) toRemove.emplace_back(item.object, total);
            }
            for (auto& [obj, count] : toRemove)
            {
                RE::TESObjectREFR::RemoveItemData data(obj, count);
                actor->RemoveItem(data);
                ++removed;
            }
        }

        spdlog::info("[Swap {:08X}] [Step 2] Stripped power-armor state from source actor "
                     "({} PA item(s) removed). Left intact, PA renders the new base invisible.",
                     a_ctx.formID,
                     removed);
    }

    // -------------------------------------------------------------------------
    // Async-chaining infrastructure
    // -------------------------------------------------------------------------

    // Forward decls for the 10 main pipeline steps and 2 retry helpers.
    void Step1_FirstDisable(SwapContext ctx);
    void Step2_SwapBase(SwapContext ctx);
    void Step3_FirstEnable(SwapContext ctx);
    void Step3_PollSettle(SwapContext ctx);
    void Step4_CheckFirst(SwapContext ctx);
    void Step4_RetryDisable(SwapContext ctx);
    void Step4_RetryEnable(SwapContext ctx);
    void Step5_EquipOutfit(SwapContext ctx);
    void Step6_FinalDisable(SwapContext ctx);
    void Step6_AfterDisable(SwapContext ctx);
    void Step7_FinalEnable(SwapContext ctx);
    void Step7_PollSettle(SwapContext ctx);
    void Step8_CheckFinal(SwapContext ctx);
    void Step9_ResetActorState(SwapContext ctx);
    void Step10_Finalize(SwapContext ctx);

    void ReleaseFaceGenLock(std::uint32_t a_targetBaseID)
    {
        if constexpr (kAllowParallelSwaps) return;

        std::function<void()> nextTask;
        {
            std::lock_guard<std::mutex> lock(g_faceGenMutex);
            auto it = g_pendingSwaps.find(a_targetBaseID);
            if (it != g_pendingSwaps.end() && !it->second.empty())
            {
                nextTask = std::move(it->second.front());
                it->second.erase(it->second.begin());
            }
            else
            {
                g_activeFaceGenBases.erase(a_targetBaseID);
                if (it != g_pendingSwaps.end()) g_pendingSwaps.erase(it);
            }
        }
        if (nextTask)
        {
            if (auto* t = F4SE::GetTaskInterface()) t->AddTask(std::move(nextTask));
        }
    }

    void FinishSwapChain(const SwapContext& a_ctx)
    {
        // Re-attach the EnableStateParent we detached at kickoff (see
        // RunPerformSwapTask). This is the single terminal for every path —
        // success and abort — so the quest enable-parent link is never left
        // dangling. AddExtra takes ownership back; if the actor vanished we
        // delete it (RemoveExtra's unique_ptr would have done the same) to
        // avoid a leak.
        if (a_ctx.savedEnableParent)
        {
            auto* form = RE::TESForm::GetFormByID(a_ctx.formID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (actor && actor->extraList)
            {
                actor->extraList->AddExtra(a_ctx.savedEnableParent);
                spdlog::info("[Swap {:08X}] Restored EnableStateParent link.", a_ctx.formID);
            }
            else
            {
                delete a_ctx.savedEnableParent;
                spdlog::warn("[Swap {:08X}] Actor gone before EnableStateParent could be "
                             "restored; discarded the saved link.",
                             a_ctx.formID);
            }
        }

        spdlog::info("[Swap {:08X}] === DONE ({}) ===", a_ctx.formID, a_ctx.ruleTag);
        ReleaseFaceGenLock(a_ctx.targetBaseID);
        if (a_ctx.onComplete) a_ctx.onComplete();
    }

    // Per-step state dump, gated on debugLevel >= 4. The calls live at every
    // step boundary; set debugLevel=4 on a rule to get the full trajectory.
    void DumpWithLabel(const SwapContext& a_ctx, const char* a_label)
    {
        if (a_ctx.debugLevel < 4) return;
        spdlog::info("");
        spdlog::info("[Swap {:08X}] ----- {} ({}) -----", a_ctx.formID, a_label, a_ctx.ruleTag);
        DumpActorInfo(a_ctx.formID);
    }

    void RunOnMain(SwapContext a_ctx, void (*a_next)(SwapContext))
    {
        if (auto* t = F4SE::GetTaskInterface())
            t->AddTask([ctx = std::move(a_ctx), a_next]() mutable { a_next(std::move(ctx)); });
        else
            FinishSwapChain(a_ctx);
    }

    void Sleep(SwapContext a_ctx, int a_ms, const char* a_reason, void (*a_next)(SwapContext))
    {
        spdlog::info("[Swap {:08X}] [wait {}ms — {}]", a_ctx.formID, a_ms, a_reason);
        DelayManager::QueueTask(a_ms, [ctx = std::move(a_ctx), a_next]() mutable { RunOnMain(std::move(ctx), a_next); });
    }

    void DispatchAwait(SwapContext a_ctx,
                       Utils::AsyncTokenRegistry<std::uint32_t>& a_registry,
                       const char* a_funcName,
                       void (*a_next)(SwapContext))
    {
        const std::uint32_t key = a_ctx.formID;
        spdlog::info("[Swap {:08X}] [dispatch BNSSpawnHelper.{} — waiting for VM callback (timeout {}ms)]",
                     key,
                     a_funcName,
                     kPapyrusAwaitTimeoutMS);
        Utils::DispatchToPapyrusAwait(
          a_registry,
          key,
          kPapyrusAwaitTimeoutMS,
          [a_ctx, a_next]() mutable { RunOnMain(std::move(a_ctx), a_next); },
          "BNSSpawnHelper",
          a_funcName,
          static_cast<std::int32_t>(key));
    }

    // True once the engine has finished the async rebuild Enable kicks off:
    // 3D loaded, facegen drained, AND the animation graph bound. The graph is
    // the part that actually matters for T-poses — facegen settling on its own
    // lets the chain finish while the skeleton is still graph-less under heavy
    // load (exactly what a later manual console Disable→Enable fixes).
    bool IsActorSettled(RE::Actor* a_actor)
    {
        if (!a_actor) return true; // vanished — don't deadlock the chain
        if (!a_actor->Get3D()) return false;

        auto* proc = a_actor->currentProcess;
        if (proc && proc->high && proc->high->faceGenLoadPending) return false;

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> graphMgr;
        if (!a_actor->GetAnimationGraphManagerImpl(graphMgr) || !graphMgr.get()) return false;

        return true;
    }

    void PollActorSettle(SwapContext a_ctx, int a_attempt, void (*a_next)(SwapContext))
    {
        auto* form = RE::TESForm::GetFormByID(a_ctx.formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;

        if (IsActorSettled(actor))
        {
            const int elapsed = a_attempt * kFaceGenPollIntervalMS;
            spdlog::info("[Swap {:08X}] [poll settle — 3D/facegen/anim-graph ready after {}ms (attempt {})]",
                         a_ctx.formID,
                         elapsed,
                         a_attempt);
            a_next(std::move(a_ctx));
            return;
        }
        if (a_attempt >= kFaceGenMaxAttempts)
        {
            spdlog::warn("[Swap {:08X}] [poll settle — TIMEOUT after {}ms, proceeding anyway]",
                         a_ctx.formID,
                         a_attempt * kFaceGenPollIntervalMS);
            a_next(std::move(a_ctx));
            return;
        }
        DelayManager::QueueTask(kFaceGenPollIntervalMS, [ctx = std::move(a_ctx), a_attempt, a_next]() mutable {
            if (auto* t = F4SE::GetTaskInterface())
                t->AddTask([ctx = std::move(ctx), a_attempt, a_next]() mutable {
                    PollActorSettle(std::move(ctx), a_attempt + 1, a_next);
                });
            else
                FinishSwapChain(ctx);
        });
    }

    // RollLVLN — walk a leveled-character tree to a concrete leaf, uniform
    // random per step. Same algorithm as the OLD pipeline.
    RE::TESBoundObject* RollLVLN(RE::TESBoundObject* a_obj, std::uint16_t a_level)
    {
        if (!a_obj) return nullptr;

        RE::TESBoundObject* current = a_obj;
        int depth = 0;

        std::random_device rd;
        std::mt19937 rng(rd());

        while (current && current->formType == RE::ENUM_FORM_ID::kLVLN && depth < 20)
        {
            auto* lev = current->As<RE::TESLevCharacter>();
            if (!lev) break;

            RE::BSScrapArray<RE::CALCED_OBJECT> calcOut;
            lev->CalculateCurrentFormList(a_level,
                                          1,
                                          calcOut,
                                          static_cast<RE::TESLeveledList::LeveledListAllBelowForce>(0),
                                          false);

            std::uint32_t validCount = 0;
            for (const auto& entry : calcOut)
                if (entry.object) validCount++;
            if (validCount == 0) return nullptr;

            std::uniform_int_distribution<std::uint32_t> dist(0, validCount - 1);
            std::uint32_t targetIdx = dist(rng);

            std::uint32_t currIdx = 0;
            for (const auto& entry : calcOut)
            {
                if (entry.object)
                {
                    if (currIdx == targetIdx)
                    {
                        current = entry.object;
                        break;
                    }
                    currIdx++;
                }
            }
            depth++;
        }

        return current;
    }

    std::uint16_t GetTargetLevel(RE::Actor* a_actor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return player ? player->GetLevel() : (a_actor ? a_actor->GetLevel() : 1);
    }

    // -------------------------------------------------------------------------
    // Pipeline steps. Each function has a DumpWithLabel(...) commented at
    // entry — uncomment when investigating a regression at that boundary.
    // -------------------------------------------------------------------------

    // Step 1 — First Papyrus Disable. Awaits the VM callback so the chain
    // doesn't advance until Disable() has actually executed.
    void Step1_FirstDisable(SwapContext a_ctx)
    {
        DumpWithLabel(a_ctx, "Step 1: BEGIN (pre-disable)");
        DispatchAwait(std::move(a_ctx), g_disableRegistry, "PapyrusDisable", &Step2_SwapBase);
    }

    // Step 2 — Replace the base form. RemoveExtra(kLeveledCreature) drops the
    // cached LVLN/template roll so the next Enable resolves cleanly from our
    // new target. EnsureRobotInstanceData materialises the templated base's
    // recipe (see helper's big WARNING for why robots break without it).
    //
    // kObjectInstance / kInstanceData must go too, and for a long time they
    // didn't. That extra holds the actor's object-template mod list — for an
    // Automatron robot it IS its body: limbs, treads, armour. Left in place it
    // survives SetObjectReference, so the actor keeps its OLD parts:
    //
    //   * robot -> organic left robot limbs attached to flesh. Measured on
    //     2026-08-14: a deathclaw with 32 instance mods, a feral ghoul with 14
    //     (visible in game as a robot thruster flame burning at its feet).
    //     Organic -> organic swaps are consistently 0, so 0 is the right value.
    //   * robot -> robot merged both lists. A Robobrain whose template declares
    //     9 includes came out with 13, wearing the source Assaultron's limbs
    //     and no treads.
    //
    // Proof it came from the source rather than the target: two swaps to the
    // SAME Warmaster base reported 10 and 0 mods depending on what the actor
    // used to be.
    //
    // The retired 9-phase pipeline cleared this in ClearLeveledAndInstanceExtras
    // ("so the surgery doesn't reuse stale data from the previous identity");
    // the v3 consolidation kept only the kLeveledCreature half.
    //
    // That old helper also cleared kInstanceData and kOutfitItem. NEITHER is
    // removed here, deliberately:
    //
    //   * kInstanceData was tried on 2026-08-14 and reverted. It fixed nothing
    //     extra — kObjectInstance alone already takes every organic target to 0
    //     mods and the Robobrain to its template's 9 — but it DID change
    //     template resolution: swaps onto template-based humans (EncGunner07/08)
    //     stopped landing on the concrete base and started minting a dynamic
    //     0xFF leaf instead. Harmless-looking, but it is a behaviour change with
    //     no problem behind it.
    //   * kOutfitItem is Step 5's business (InitDefaultWornImpl), and nothing
    //     observed suggests it goes stale.
    //
    // Removing extras here is not free. Only remove one with evidence.
    void Step2_SwapBase(SwapContext a_ctx)
    {
        DumpWithLabel(a_ctx, "Step 2: AFTER first Disable, BEFORE SwapBase");

        auto* form = RE::TESForm::GetFormByID(a_ctx.formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor)
        {
            spdlog::warn("[Swap {:08X}] [Step 2] Actor vanished; aborting.", a_ctx.formID);
            FinishSwapChain(a_ctx);
            return;
        }

        spdlog::info("[Swap {:08X}] [Step 2] RemoveExtra(kLeveledCreature, kObjectInstance) + "
                     "SetObjectReference('{}' [{:08X}]) + EnsureRobotInstanceData",
                     a_ctx.formID,
                     Utils::GetFormName(a_ctx.targetBase),
                     a_ctx.targetBaseID);

        if (actor->extraList)
        {
            actor->extraList->RemoveExtra(RE::EXTRA_DATA_TYPE::kLeveledCreature);
            // Order matters: this must go BEFORE SetObjectReference, so that
            // EnsureRobotInstanceData below builds the new base's recipe onto a
            // clean slate instead of merging into the old identity's.
            actor->extraList->RemoveExtra(RE::EXTRA_DATA_TYPE::kObjectInstance);
        }
        StripPowerArmorState(a_ctx);
        actor->SetObjectReference(a_ctx.targetBase);
        EnsureRobotInstanceData(a_ctx);

        Sleep(std::move(a_ctx), kBetweenStepGuessMS, "settle before first Enable", &Step3_FirstEnable);
    }

    // Step 3 — First Papyrus Enable. The engine does the heavy lifting:
    // resolves templates, generates a 0xFF leaf base, sets race, rebuilds 3D
    // / biped / AIProcess, and ADDS (but doesn't equip — Step 5 does) the
    // default-outfit items. Then poll facegen-settle.
    void Step3_FirstEnable(SwapContext a_ctx)
    {
        DumpWithLabel(a_ctx, "Step 3: AFTER SwapBase, BEFORE first Enable");
        DispatchAwait(std::move(a_ctx), g_enableRegistry, "PapyrusEnable", &Step3_PollSettle);
    }

    void Step3_PollSettle(SwapContext a_ctx) { PollActorSettle(std::move(a_ctx), 0, &Step4_CheckFirst); }

    // Step 4 — Verify the engine actually finished resolving. If it bailed
    // mid-resolution (legendary case), retry the cycle with a bare
    // Disable→Enable (no SetObjectReference — base is already pinned).
    // Bounded by kMaxResolveRetries.
    void Step4_CheckFirst(SwapContext a_ctx)
    {
        DumpWithLabel(a_ctx, "Step 4: AFTER first Enable (settled)");

        std::string reason;
        if (IsResolutionBroken(a_ctx.formID, reason))
        {
            if (a_ctx.resolveRetries < kMaxResolveRetries)
            {
                a_ctx.resolveRetries++;
                spdlog::warn("[Swap {:08X}] Broken resolution: {}. Retrying Disable→Enable "
                             "(attempt {}/{}).",
                             a_ctx.formID,
                             reason,
                             a_ctx.resolveRetries,
                             kMaxResolveRetries);
                RunOnMain(std::move(a_ctx), &Step4_RetryDisable);
                return;
            }
            spdlog::warn("[Swap {:08X}] Broken resolution still present after {} retries: {}. "
                         "Proceeding anyway (actor may render as floating head).",
                         a_ctx.formID,
                         kMaxResolveRetries,
                         reason);
        }

        RunOnMain(std::move(a_ctx), &Step5_EquipOutfit);
    }

    // Step 4 retry: bare Disable → bare Enable → re-poll → re-check.
    void Step4_RetryDisable(SwapContext a_ctx)
    { DispatchAwait(std::move(a_ctx), g_disableRegistry, "PapyrusDisable", &Step4_RetryEnable); }

    void Step4_RetryEnable(SwapContext a_ctx)
    {
        // Step4_RetryDisable just stripped the templated base's instance data.
        // Re-create now or robots come back invisible. See EnsureRobotInstanceData.
        EnsureRobotInstanceData(a_ctx);
        DispatchAwait(std::move(a_ctx), g_enableRegistry, "PapyrusEnable", &Step3_PollSettle);
    }

    // Step 5 — Equip the new base's default outfit. InitDefaultWornImpl is
    // what fresh-spawn uses; covers both "Enable added items but didn't
    // equip" and "robot template left inventory empty" cases.
    void Step5_EquipOutfit(SwapContext a_ctx)
    {
        spdlog::info("[Swap {:08X}] [Step 5] InitDefaultWornImpl(true, true)", a_ctx.formID);

        auto* form = RE::TESForm::GetFormByID(a_ctx.formID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor)
        {
            spdlog::warn("[Swap {:08X}] [Step 5] Actor vanished; aborting.", a_ctx.formID);
            FinishSwapChain(a_ctx);
            return;
        }
        actor->InitDefaultWornImpl(true, true);

        Sleep(std::move(a_ctx),
              kBeforeFinalDisableMS,
              "let InitDefaultWornImpl settle, then final Disable",
              &Step6_FinalDisable);
    }

    // Step 6 — Final Disable. Together with Step 7's Enable, this is what
    // actually clears T-poses. Reset3D / ResetHavokPhysics /
    // HandleDefaultAnimationSwitch were tried as substitutes and did nothing.
    // The cost is another template re-roll; Step 8 catches breakage.
    void Step6_FinalDisable(SwapContext a_ctx)
    {
        DumpWithLabel(a_ctx, "Step 6: AFTER equip, BEFORE final Disable");
        DispatchAwait(std::move(a_ctx), g_disableRegistry, "PapyrusDisable", &Step6_AfterDisable);
    }

    // Gap between the final Disable and Enable — lets the engine finish tearing
    // 3D down before Step 7 rebuilds it (mimics the multi-second gap of a manual
    // console Disable→Enable). See kBetweenFinalDisableEnableMS.
    void Step6_AfterDisable(SwapContext a_ctx)
    {
        Sleep(std::move(a_ctx),
              kBetweenFinalDisableEnableMS,
              "settle after final Disable, before final Enable",
              &Step7_FinalEnable);
    }

    // Step 7 — Final Enable. EnsureRobotInstanceData re-runs because Step 6's
    // Disable stripped it. Then poll facegen-settle.
    void Step7_FinalEnable(SwapContext a_ctx)
    {
        DumpWithLabel(a_ctx, "Step 7: AFTER final Disable, BEFORE final Enable");
        EnsureRobotInstanceData(a_ctx);
        DispatchAwait(std::move(a_ctx), g_enableRegistry, "PapyrusEnable", &Step7_PollSettle);
    }

    void Step7_PollSettle(SwapContext a_ctx) { PollActorSettle(std::move(a_ctx), 0, &Step8_CheckFinal); }

    // Step 8 — Same broken-resolution guard as Step 4. If the final cycle's
    // re-roll landed on a legendary leaf, loop back to Step 6.
    void Step8_CheckFinal(SwapContext a_ctx)
    {
        DumpWithLabel(a_ctx, "Step 8: AFTER final Enable (settled)");

        std::string reason;
        if (IsResolutionBroken(a_ctx.formID, reason))
        {
            if (a_ctx.finalRetries < kMaxResolveRetries)
            {
                a_ctx.finalRetries++;
                spdlog::warn("[Swap {:08X}] Broken resolution after final Enable: {}. "
                             "Retrying final Disable→Enable (attempt {}/{}).",
                             a_ctx.formID,
                             reason,
                             a_ctx.finalRetries,
                             kMaxResolveRetries);
                RunOnMain(std::move(a_ctx), &Step6_FinalDisable);
                return;
            }
            spdlog::warn("[Swap {:08X}] Broken resolution still present after {} final retries: {}. "
                         "Proceeding anyway (actor may render as floating head).",
                         a_ctx.formID,
                         kMaxResolveRetries,
                         reason);
        }

        RunOnMain(std::move(a_ctx), &Step9_ResetActorState);
    }

    // Step 9 — Papyrus ResetActorState (HP/limbs only — see BNSSpawnHelper.psc).
    // It used to do Disable+Enable+ResetHealthAndLimbs; that introduced a
    // third uncontrolled template re-roll, which became visible as floating
    // heads. The await still matters under heavy load so Step 10 doesn't fire
    // while the VM is still draining.
    void Step9_ResetActorState(SwapContext a_ctx)
    { DispatchAwait(std::move(a_ctx), g_resetRegistry, "ResetActorState", &Step10_Finalize); }

    // Step 10 — Re-pin pose (the Disable→Enable cycles nudge by ~50 units),
    // release the per-base face-gen lock, fire onComplete. NPCModifier runs
    // next.
    void Step10_Finalize(SwapContext a_ctx)
    {
        if (auto* form = RE::TESForm::GetFormByID(a_ctx.formID))
        {
            if (auto* actor = form->As<RE::Actor>())
            {
                actor->SetPosition(a_ctx.startPos, true);
                Utils::SetActorAngle(actor, a_ctx.startRot);
            }
        }
        DumpWithLabel(a_ctx, "Step 10: FINAL (after re-pin, swap complete)");
        FinishSwapChain(a_ctx);
    }

    // -------------------------------------------------------------------------
    // Kickoff
    // -------------------------------------------------------------------------

    void KickoffOrQueueSwap(SwapContext a_ctx)
    {
        if constexpr (kAllowParallelSwaps)
        {
            Step1_FirstDisable(std::move(a_ctx));
            return;
        }

        const std::uint32_t targetBaseID = a_ctx.targetBaseID;
        {
            std::lock_guard<std::mutex> lock(g_faceGenMutex);
            if (g_activeFaceGenBases.contains(targetBaseID))
            {
                g_pendingSwaps[targetBaseID].push_back(
                  [ctx = std::move(a_ctx)]() mutable { Step1_FirstDisable(std::move(ctx)); });
                return;
            }
            g_activeFaceGenBases.insert(targetBaseID);
        }
        Step1_FirstDisable(std::move(a_ctx));
    }

    void RunPerformSwapTask(std::uint32_t a_refID, SwapRuleResolved a_rule, std::function<void()> a_onComplete)
    {
        auto* form = RE::TESForm::GetFormByID(a_refID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor)
        {
            spdlog::debug("[Swap] Actor [{:08X}] not found at entry — skipping.", a_refID);
            if (a_onComplete) a_onComplete();
            return;
        }

        auto* newBase = a_rule.replaceBy ? a_rule.replaceBy->As<RE::TESBoundObject>() : nullptr;
        if (!newBase)
        {
            spdlog::warn("[Swap] Rule '{}' replaceBy is not a TESBoundObject — skipping.", a_rule.ruleName);
            if (a_onComplete) a_onComplete();
            return;
        }

        const std::uint16_t level = GetTargetLevel(actor);
        RE::TESBoundObject* targetBase = newBase;
        if (targetBase->formType == RE::ENUM_FORM_ID::kLVLN)
        {
            targetBase = RollLVLN(targetBase, level);
            spdlog::info("[Swap {:08X}] Rolled LVLN '{}' [{:08X}] -> '{}' [{:08X}]",
                         a_refID,
                         Utils::GetFormName(newBase),
                         newBase->GetFormID(),
                         targetBase ? Utils::GetFormName(targetBase) : "<null>",
                         targetBase ? targetBase->GetFormID() : 0);
        }
        if (!targetBase || targetBase->formType != RE::ENUM_FORM_ID::kNPC_)
        {
            spdlog::warn("[Swap {:08X}] Could not resolve a concrete NPC base; skipping.", a_refID);
            if (a_onComplete) a_onComplete();
            return;
        }

        if (RE::TESForm* oldBase = Utils::GetTrueBaseForm(actor); oldBase == targetBase)
        {
            if (a_rule.debugLevel >= 1) spdlog::debug("[Swap {:08X}] Target equals current base — skipping.", a_refID);
            if (a_onComplete) a_onComplete();
            return;
        }

        SwapContext ctx;
        ctx.formID = a_refID;
        ctx.targetBaseID = targetBase->GetFormID();
        ctx.debugLevel = a_rule.debugLevel;
        ctx.targetBase = targetBase;
        ctx.ruleTag = std::format("{} | {}", a_rule.sourceFile, a_rule.ruleName);
        ctx.startPos = actor->GetPosition();
        ctx.startRot = actor->data.angle;
        ctx.onComplete = std::move(a_onComplete);

        spdlog::info("[Swap {:08X}] === BEGIN ({}) target='{}' [{:08X}] ===",
                     ctx.formID,
                     ctx.ruleTag,
                     Utils::GetFormName(targetBase),
                     ctx.targetBaseID);

        // Detach the EnableStateParent (XESP) link BEFORE the first Disable. An
        // enable-parented ref ignores Papyrus Disable (it stays fully loaded —
        // Get3D non-null after the "Disable") and its Enable then skips template
        // resolution → SynthGen2 floating head. Removing the link makes the
        // whole Disable/Enable cycle behave like a plain ref. RemoveExtra hands
        // us OWNERSHIP (it does not free), so we keep the exact extra — parent
        // ref + flags preserved — and FinishSwapChain re-attaches it. Main-thread
        // here (PerformSwap queues this via the F4SE task interface).
        if (actor->extraList && actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kEnableStateParent))
        {
            ctx.savedEnableParent = actor->extraList->RemoveExtra(RE::EXTRA_DATA_TYPE::kEnableStateParent).release();
            // ⚠ CRITICAL: BaseExtraList::RemoveExtra unlinks the node but does NOT
            // clear its `next` pointer — it still points at whatever followed it in
            // the list. Normally harmless (the returned unique_ptr just deletes it),
            // but we .release() and re-AddExtra this exact node in FinishSwapChain.
            // AddExtra's tail-append branch (kEnableStateParent is not "high-use")
            // relies on the contract `next == nullptr` (a release-build-disabled
            // assert) and does NOT clear it either, so re-attaching would splice the
            // STALE next back into the list. That successor gets freed during the
            // swap's churn (RemoveExtra(kLeveledCreature), instance-data creation,
            // engine Disable/Enable rebuilds), leaving a dangling node. Gameplay
            // survives (GetByType/HasType short-circuit), but ExtraDataList::SaveGame
            // walks the whole list on save → use-after-free → crash. Null it now to
            // restore the AddExtra precondition.
            if (ctx.savedEnableParent) ctx.savedEnableParent->next = nullptr;
            spdlog::info("[Swap {:08X}] Detached EnableStateParent for the swap "
                         "(will restore on completion).",
                         ctx.formID);
        }

        KickoffOrQueueSwap(std::move(ctx));
    }

} // anonymous namespace

void OnResetActorStateDone(std::uint32_t a_refID)
{
    if (auto cb = g_resetRegistry.ConsumeOldest(a_refID); cb) cb();
}

void OnPapyrusDisableDone(std::uint32_t a_refID)
{
    if (auto cb = g_disableRegistry.ConsumeOldest(a_refID); cb) cb();
}

void OnPapyrusEnableDone(std::uint32_t a_refID)
{
    if (auto cb = g_enableRegistry.ConsumeOldest(a_refID); cb) cb();
}

void PerformSwap(std::uint32_t a_refID, const SwapRuleResolved& rule, std::function<void()> a_onComplete)
{
    auto* task = F4SE::GetTaskInterface();
    if (!task)
    {
        if (a_onComplete) a_onComplete();
        return;
    }

    task->AddTask([a_refID, rule, onComplete = std::move(a_onComplete)]() mutable {
        RunPerformSwapTask(a_refID, std::move(rule), std::move(onComplete));
    });
}

// =============================================================================
// DumpActorInfo — console-callable diagnostic. Reachable via
//   cgf "BNSSpawnHelper.DumpInfo" <refID>
// Also used by the commented-out DumpWithLabel calls at the top of each step.
// =============================================================================
namespace
{
    const char* TemplateFlagsStr(RE::TESActorBase* a_base)
    {
        static thread_local char buf[16];
        if (!a_base) return "<no-base>";
        const auto raw = static_cast<std::uint32_t>(a_base->actorData.templateUseFlags.underlying());
        for (int i = 0; i < 13; ++i)
            buf[12 - i] = (raw & (1u << i)) ? '1' : '0';
        buf[13] = '\0';
        return buf;
    }

    bool IsFemale(RE::TESNPC* a_npc)
    {
        if (!a_npc) return false;
        return a_npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kFemale);
    }
} // anonymous namespace

void DumpActorInfo(std::uint32_t a_refID)
{
    auto* form = RE::TESForm::GetFormByID(a_refID);
    auto* actor = form ? form->As<RE::Actor>() : nullptr;
    if (!actor)
    {
        spdlog::info("=== BNS DumpInfo: [{:08X}] is not a loaded Actor ===", a_refID);
        return;
    }

    auto* base = actor->data.objectReference;
    auto* baseAsActorBase = base ? base->As<RE::TESActorBase>() : nullptr;
    auto* baseAsNPC = base ? base->As<RE::TESNPC>() : nullptr;
    auto* baseRace = baseAsNPC ? baseAsNPC->formRace : nullptr;

    const std::uint32_t baseID = base ? base->GetFormID() : 0;
    const bool baseIs0xFF = ((baseID >> 24) == 0xFF);
    const auto& pos = actor->data.location;
    const auto& rot = actor->data.angle;

    spdlog::info("=== BNS DumpInfo: actor=[{:08X}] ===", actor->GetFormID());
    spdlog::info("  formFlags         = 0x{:08X}", actor->GetFormFlags());
    spdlog::info("  pos               = ({:.1f}, {:.1f}, {:.1f})", pos.x, pos.y, pos.z);
    spdlog::info("  rot               = ({:.3f}, {:.3f}, {:.3f})", rot.x, rot.y, rot.z);
    spdlog::info("  base              = '{}' [{:08X}] {} templateFlags={}",
                 base ? Utils::GetFormName(base) : "<null>",
                 baseID,
                 baseIs0xFF ? "(0xFF leaf)" : "(hub)",
                 TemplateFlagsStr(baseAsActorBase));
    if (baseAsNPC)
    {
        spdlog::info("  base flags        = female={} unique={} essential={}",
                     IsFemale(baseAsNPC),
                     baseAsNPC->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kUnique),
                     baseAsNPC->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kEssential));
    }
    spdlog::info("  actor->race       = '{}' [{:08X}]",
                 actor->race ? Utils::GetFormName(actor->race) : "<null>",
                 actor->race ? actor->race->GetFormID() : 0);
    spdlog::info("  base->formRace    = '{}' [{:08X}]",
                 baseRace ? Utils::GetFormName(baseRace) : "<null>",
                 baseRace ? baseRace->GetFormID() : 0);
    spdlog::info("  biped             = {}    Get3D() = {}",
                 actor->biped ? "present" : "null",
                 actor->Get3D() ? "present" : "null");
    spdlog::info("  raceSwitchPending = {}", actor->raceSwitchPending ? "set" : "clear");
    spdlog::info("  IsDead(true)      = {}", actor->IsDead(true) ? "true" : "false");

    auto* proc = actor->currentProcess;
    spdlog::info("  currentProcess    = {} (high={} middleHigh={} middleLow={})",
                 proc ? "present" : "null",
                 (proc && proc->high) ? "set" : "null",
                 (proc && proc->middleHigh) ? "set" : "null",
                 (proc && proc->middleLow) ? "set" : "null");

    RE::ExtraLeveledCreature* lvlExtra = nullptr;
    if (actor->extraList) { lvlExtra = actor->extraList->GetByType<RE::ExtraLeveledCreature>(); }
    spdlog::info("  ExtraLeveledCreature = {}{}",
                 lvlExtra ? "present" : "absent",
                 (lvlExtra && lvlExtra->originalBase) ? std::format(" (originalBase='{}' [{:08X}])",
                                                                    Utils::GetFormName(lvlExtra->originalBase),
                                                                    lvlExtra->originalBase->GetFormID())
                                                      : std::string {});
    if (lvlExtra)
    {
        for (std::uint32_t i = 0; i < 13; ++i)
        {
            RE::TESActorBase* slot = lvlExtra->templates[i];
            if (!slot) continue;
            auto* slotAsNPC = slot->As<RE::TESNPC>();
            auto* slotRace = slotAsNPC ? slotAsNPC->formRace : nullptr;
            spdlog::info("    templates[{:2}] = '{}' [{:08X}]  race='{}' [{:08X}]  templateFlags={}",
                         i,
                         Utils::GetFormName(slot),
                         slot->GetFormID(),
                         slotRace ? Utils::GetFormName(slotRace) : "<null>",
                         slotRace ? slotRace->GetFormID() : 0,
                         TemplateFlagsStr(slot));
        }
    }

    if (actor->inventoryList)
    {
        spdlog::info("  inventory ({} entries):", actor->inventoryList->data.size());
        for (const auto& item : actor->inventoryList->data)
        {
            if (!item.object) continue;
            std::uint32_t totalCount = 0;
            bool equipped = false;
            auto stack = item.stackData;
            while (stack)
            {
                totalCount += static_cast<std::uint32_t>(stack->count);
                if (stack->IsEquipped()) equipped = true;
                stack = stack->nextStack;
            }
            spdlog::info("    [{:08X}] '{}' x{} {}",
                         item.object->GetFormID(),
                         Utils::GetFormName(item.object),
                         totalCount,
                         equipped ? "[EQUIPPED]" : "");
        }
    }
    else
    {
        spdlog::info("  inventory         = <no inventoryList>");
    }

    if (actor->extraList)
    {
        struct
        {
            RE::EXTRA_DATA_TYPE type;
            const char* name;
        } kTracked[] = {
          {RE::EXTRA_DATA_TYPE::kEditorID, "kEditorID"},
          {RE::EXTRA_DATA_TYPE::kRaceData, "kRaceData"},
          {RE::EXTRA_DATA_TYPE::kInstanceData, "kInstanceData"},
          {RE::EXTRA_DATA_TYPE::kModelSwap, "kModelSwap"},
          {RE::EXTRA_DATA_TYPE::kPowerArmor, "kPowerArmor"},
          {RE::EXTRA_DATA_TYPE::kInactivePowerArmor, "kInactivePowerArmor"},
          {RE::EXTRA_DATA_TYPE::kPowerArmorPreload, "kPowerArmorPreload"},
          {RE::EXTRA_DATA_TYPE::kOutfitItem, "kOutfitItem"},
          {RE::EXTRA_DATA_TYPE::kLeveledCreature, "kLeveledCreature"},
          {RE::EXTRA_DATA_TYPE::kFactionChanges, "kFactionChanges"},
          {RE::EXTRA_DATA_TYPE::kTextDisplayData, "kTextDisplayData"},
          {RE::EXTRA_DATA_TYPE::kBiped, "kBiped"},
          {RE::EXTRA_DATA_TYPE::kHealth, "kHealth"},
          {RE::EXTRA_DATA_TYPE::kReferenceHandle, "kReferenceHandle"},
          {RE::EXTRA_DATA_TYPE::kStartingWorldOrCell, "kStartingWorldOrCell"},
          {RE::EXTRA_DATA_TYPE::kVoiceType, "kVoiceType"},
          {RE::EXTRA_DATA_TYPE::kObjectInstance, "kObjectInstance"},
          {RE::EXTRA_DATA_TYPE::kEnableStateParent, "kEnableStateParent"},
          {RE::EXTRA_DATA_TYPE::kFavorite, "kFavorite"},
          {RE::EXTRA_DATA_TYPE::kEnchantment, "kEnchantment"},
          {RE::EXTRA_DATA_TYPE::kAmmo, "kAmmo"},
          {RE::EXTRA_DATA_TYPE::kCount, "kCount"},
          {RE::EXTRA_DATA_TYPE::kCharge, "kCharge"},
          {RE::EXTRA_DATA_TYPE::kSavedHavokData, "kSavedHavokData"},
          {RE::EXTRA_DATA_TYPE::kRagdollData, "kRagdollData"},
          {RE::EXTRA_DATA_TYPE::kAnim, "kAnim"},
          {RE::EXTRA_DATA_TYPE::kHavokAnim, "kHavokAnim"},
          {RE::EXTRA_DATA_TYPE::kSavedAnimation, "kSavedAnimation"},
          {RE::EXTRA_DATA_TYPE::kAnimSequencer, "kAnimSequencer"},
          {RE::EXTRA_DATA_TYPE::kCachedScale, "kCachedScale"},
          {RE::EXTRA_DATA_TYPE::kCulledBone, "kCulledBone"},
          {RE::EXTRA_DATA_TYPE::kBoundArmor, "kBoundArmor"},
        };
        std::string joined;
        for (const auto& e : kTracked)
        {
            if (actor->extraList->HasType(e.type))
            {
                if (!joined.empty()) joined += ", ";
                joined += e.name;
            }
        }
        if (joined.empty())
            spdlog::info("  extraData         = <none of the tracked types present>");
        else
            spdlog::info("  extraData         = {}", joined);
    }
    else
    {
        spdlog::info("  extraData         = <no extraList>");
    }

    spdlog::info("=== End DumpInfo for [{:08X}] ===", actor->GetFormID());
}

} // namespace BNS::Swapper
