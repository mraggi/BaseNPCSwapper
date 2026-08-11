#include "MNAMResolver.hpp"
#include "Utils.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BNS::MNAMResolver
{

namespace
{

    std::unordered_map<RE::TESFormID, RE::BGSKeyword*> g_cache;

    // Release on MarkCacheReady, acquire on IsCacheReady — a true read
    // guarantees the per-rule OMODCache writes done before the flip are
    // visible.
    std::atomic<bool> g_cacheReady {false};

    constexpr std::uint32_t FourCC(const char (&s)[5])
    {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0]))
          | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8)
          | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16)
          | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24);
    }

    constexpr auto kTagTES4 = FourCC("TES4");
    constexpr auto kTagGRUP = FourCC("GRUP");
    constexpr auto kTagOMOD = FourCC("OMOD");
    constexpr auto kTagMNAM = FourCC("MNAM");
    constexpr auto kTagXXXX = FourCC("XXXX");
    constexpr std::uint32_t kFlagCompressed = 0x00040000u;

    // 24-byte fixed header — same shape for forms and GRUPs.
    struct RecHeader
    {
        std::uint32_t type;
        std::uint32_t dataSize; // forms: payload only; GRUPs: INCLUDES the 24-byte header
        std::uint32_t flags; // GRUP: label (e.g. 'OMOD' for top OMOD group)
        std::uint32_t formID; // GRUP: groupType (0 = top)
        std::uint32_t vc1;
        std::uint16_t formVersion;
        std::uint16_t vc2;
    };
    static_assert(sizeof(RecHeader) == 24);

    std::string TagStr(std::uint32_t t)
    {
        char c[5] = {0};
        for (int i = 0; i < 4; ++i)
        {
            const char ch = static_cast<char>((t >> (i * 8)) & 0xFF);
            c[i] = (ch >= 0x20 && ch <= 0x7E) ? ch : '.';
        }
        return std::string(c);
    }

    bool ReadHeader(std::ifstream& a_s, RecHeader& a_h)
    {
        a_s.read(reinterpret_cast<char*>(&a_h), sizeof(a_h));
        return static_cast<std::size_t>(a_s.gcount()) == sizeof(a_h);
    }

    // File-relative → runtime FormID. In a plugin file, FormID's high byte
    // indexes into the masters list (high_byte == masterCount → own form).
    RE::TESFormID ApplyRuntimeIndex(RE::TESFile* a_file, RE::TESFormID a_local)
    {
        if (a_file->IsLight())
        {
            return 0xFE000000u | (static_cast<RE::TESFormID>(a_file->GetSmallFileCompileIndex()) << 12)
              | (a_local & 0xFFFu);
        }
        return (static_cast<RE::TESFormID>(a_file->GetCompileIndex()) << 24) | (a_local & 0xFFFFFFu);
    }

    RE::TESFormID ResolveFileFormID(RE::TESFile* a_file, RE::TESFormID a_fileFormID)
    {
        if (!a_file) return 0;

        const std::uint8_t highByte = static_cast<std::uint8_t>(a_fileFormID >> 24);
        const RE::TESFormID localPart = a_fileFormID & 0x00FFFFFFu;

        if (highByte < a_file->masterCount)
        {
            if (!a_file->masterPtrs) return 0;
            RE::TESFile* master = a_file->masterPtrs[highByte];
            return master ? ApplyRuntimeIndex(master, localPart) : 0;
        }
        return ApplyRuntimeIndex(a_file, localPart);
    }

    RE::TESFile* FindSourceFile(RE::TESFormID a_formID)
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return nullptr;
        for (auto* f : dh->compiledFileCollection.smallFiles)
        {
            if (f && f->IsFormInMod(a_formID)) return f;
        }
        for (auto* f : dh->compiledFileCollection.files)
        {
            if (f && f->IsFormInMod(a_formID)) return f;
        }
        return nullptr;
    }

    std::filesystem::path BuildFilePath(RE::TESFile* a_file)
    {
        const char* dir = a_file->path;
        const char* file = a_file->filename;

        if (!file || !file[0])
        {
            spdlog::warn("[MNAM]   filename missing");
            return {};
        }

        if (dir && dir[0])
        {
            auto p = std::filesystem::path(dir) / file;
            if (std::filesystem::is_regular_file(p)) return p;
        }

        auto p = std::filesystem::path("Data") / file;
        if (std::filesystem::is_regular_file(p)) return p;

        spdlog::warn("[MNAM]   couldn't resolve path for '{}'", file);
        return {};
    }

    // One OMOD record's payload → its MNAM array.
    std::vector<RE::TESFormID> ExtractMNAMFromPayload(const std::vector<char>& a_buf)
    {
        std::vector<RE::TESFormID> out;
        std::size_t pos = 0;
        std::uint32_t xxxxOverride = 0;

        while (pos + 6 <= a_buf.size())
        {
            const auto subType = *reinterpret_cast<const std::uint32_t*>(&a_buf[pos]);
            const auto subSize = *reinterpret_cast<const std::uint16_t*>(&a_buf[pos + 4]);
            pos += 6;

            const std::size_t realSize = xxxxOverride ? xxxxOverride : subSize;
            xxxxOverride = 0;

            if (pos + realSize > a_buf.size()) break;

            if (subType == kTagXXXX)
            {
                if (realSize == sizeof(std::uint32_t))
                {
                    xxxxOverride = *reinterpret_cast<const std::uint32_t*>(&a_buf[pos]);
                }
                pos += realSize;
                continue;
            }

            if (subType == kTagMNAM)
            {
                const std::size_t count = realSize / sizeof(RE::TESFormID);
                out.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    out.push_back(*reinterpret_cast<const RE::TESFormID*>(&a_buf[pos + i * sizeof(RE::TESFormID)]));
                }
                return out;
            }

            pos += realSize;
        }
        return out;
    }

    void RegisterMNAM(RE::TESFile* a_file, RE::TESFormID a_omodFileID, RE::TESFormID a_kwFileID)
    {
        const auto runtimeOMOD = ResolveFileFormID(a_file, a_omodFileID);
        const auto runtimeKw = ResolveFileFormID(a_file, a_kwFileID);

        auto* kwForm = RE::TESForm::GetFormByID(runtimeKw);
        auto* kw = kwForm ? kwForm->As<RE::BGSKeyword>() : nullptr;

        if (!kw)
        {
            spdlog::warn("[MNAM]   OMOD[file:{:08X}->rt:{:08X}]  MNAM[file:{:08X}->rt:{:08X}]  -> non-keyword",
                         a_omodFileID,
                         runtimeOMOD,
                         a_kwFileID,
                         runtimeKw);
            return;
        }

        g_cache.emplace(runtimeOMOD, kw);
        // spdlog::debug("[MNAM]   OMOD [{:08X}] -> '{}' [{:08X}]",runtimeOMOD, BNS::Utils::GetFormName(kw),
        // kw->GetFormID());
    }

    // Only acts on OMODs in the needed set; others are seek'd past.
    void ProcessOMODRecord(RE::TESFile* a_file,
                           std::ifstream& a_s,
                           const RecHeader& a_h,
                           const std::unordered_set<RE::TESFormID>& a_needed)
    {
        const auto runtimeID = ResolveFileFormID(a_file, a_h.formID);

        // Skip OMODs we don't care about - parse nothing, just seek past
        if (!a_needed.contains(runtimeID))
        {
            a_s.seekg(a_h.dataSize, std::ios::cur);
            return;
        }

        if (a_h.flags & kFlagCompressed)
        {
            spdlog::warn("[MNAM] OMOD [{:08X}] is compressed - skipping "
                         "(zlib decompression not wired)",
                         runtimeID);
            a_s.seekg(a_h.dataSize, std::ios::cur);
            return;
        }

        std::vector<char> payload(a_h.dataSize);
        a_s.read(payload.data(), a_h.dataSize);
        if (static_cast<std::uint32_t>(a_s.gcount()) != a_h.dataSize)
        {
            spdlog::warn("[MNAM] short read on OMOD [{:08X}]", runtimeID);
            return;
        }

        const auto kws = ExtractMNAMFromPayload(payload);
        if (kws.empty())
        {
            spdlog::debug("[MNAM]   OMOD [{:08X}] -> (no MNAM in record)", runtimeID);
            return;
        }

        for (const auto kwLocal : kws)
        {
            RegisterMNAM(a_file, a_h.formID, kwLocal);
        }
    }

    void ProcessOMODGroupBody(RE::TESFile* a_file,
                              std::ifstream& a_s,
                              std::uint32_t a_bodyBytes,
                              const std::unordered_set<RE::TESFormID>& a_needed)
    {
        std::uint32_t consumed = 0;
        while (consumed + sizeof(RecHeader) <= a_bodyBytes)
        {
            RecHeader h;
            if (!ReadHeader(a_s, h)) break;
            consumed += sizeof(RecHeader);

            if (h.type == kTagGRUP)
            {
                const std::uint32_t inner = h.dataSize - sizeof(RecHeader);
                a_s.seekg(inner, std::ios::cur);
                consumed += inner;
                continue;
            }

            if (h.type != kTagOMOD)
            {
                a_s.seekg(h.dataSize, std::ios::cur);
                consumed += h.dataSize;
                continue;
            }

            ProcessOMODRecord(a_file, a_s, h, a_needed);
            consumed += h.dataSize;
        }
    }

    void ScanFile(RE::TESFile* a_file, const std::unordered_set<RE::TESFormID>& a_needed)
    {
        if (!a_file) return;

        const auto path = BuildFilePath(a_file);
        if (path.empty()) return;

        std::ifstream s(path, std::ios::binary);
        if (!s)
        {
            spdlog::warn("[MNAM] failed to open '{}'", path.string());
            return;
        }

        RecHeader tes4;
        if (!ReadHeader(s, tes4) || tes4.type != kTagTES4)
        {
            spdlog::warn("[MNAM] '{}' doesn't begin with TES4 (got '{}')", path.string(), TagStr(tes4.type));
            return;
        }
        s.seekg(tes4.dataSize, std::ios::cur);

        const std::size_t before = g_cache.size();

        RecHeader gh;
        while (ReadHeader(s, gh))
        {
            if (gh.type != kTagGRUP)
            {
                spdlog::warn("[MNAM] expected GRUP, got '{}' (0x{:08X}). Stopping.", TagStr(gh.type), gh.type);
                break;
            }

            const auto bodyBytes = gh.dataSize - static_cast<std::uint32_t>(sizeof(RecHeader));

            if (gh.flags == kTagOMOD && gh.formID == 0) { ProcessOMODGroupBody(a_file, s, bodyBytes, a_needed); }
            else
            {
                s.seekg(bodyBytes, std::ios::cur);
            }
        }

        const auto added = g_cache.size() - before;
        spdlog::info("[MNAM] '{}': +{} MNAMs cached", a_file->GetFilename(), added);
    }

    struct GatherResult
    {
        std::unordered_set<RE::TESFile*> files;
        std::unordered_set<RE::TESFormID> neededOMODs;
    };

    GatherResult Gather(const std::vector<SwapRuleResolved>& a_rules, bool a_skipAlreadyCached)
    {
        GatherResult r;
        for (const auto& rule : a_rules)
        {
            for (auto* omod : rule.addOMODs)
            {
                if (!omod) continue;
                const auto id = omod->GetFormID();
                if (a_skipAlreadyCached && g_cache.contains(id)) continue;
                r.neededOMODs.insert(id);
                if (auto* f = FindSourceFile(id)) r.files.insert(f);
            }
        }
        return r;
    }

} // namespace

void BuildCache(const std::vector<SwapRuleResolved>& a_rules)
{
    g_cache.clear();

    const auto gathered = Gather(a_rules, /*skipAlreadyCached=*/false);
    spdlog::info("[MNAM] Resolving MNAMs for {} OMODs from {} files", gathered.neededOMODs.size(), gathered.files.size());

    for (auto* f : gathered.files)
    {
        ScanFile(f, gathered.neededOMODs);
    }

    spdlog::info("[MNAM] Cache built: {} OMOD -> keyword entries", g_cache.size());
}

void BuildCacheIncremental(const std::vector<SwapRuleResolved>& a_rules)
{
    const auto gathered = Gather(a_rules, /*skipAlreadyCached=*/true);
    if (gathered.neededOMODs.empty())
    {
        spdlog::info("[MNAM] Incremental: no new OMODs to resolve.");
        return;
    }
    spdlog::info("[MNAM] Incremental: {} new OMODs across {} file(s)", gathered.neededOMODs.size(), gathered.files.size());

    for (auto* f : gathered.files)
    {
        ScanFile(f, gathered.neededOMODs);
    }

    spdlog::info("[MNAM] Cache now holds {} OMOD -> keyword entries", g_cache.size());
}

bool IsCacheReady() { return g_cacheReady.load(std::memory_order_acquire); }
void MarkCacheReady() { g_cacheReady.store(true, std::memory_order_release); }

RE::BGSKeyword* GetMNAM(RE::BGSMod::Attachment::Mod* a_omod) { return a_omod ? GetMNAM(a_omod->GetFormID()) : nullptr; }

RE::BGSKeyword* GetMNAM(RE::TESFormID a_omodFormID)
{
    auto it = g_cache.find(a_omodFormID);
    return it != g_cache.end() ? it->second : nullptr;
}

} // namespace BNS::MNAMResolver
