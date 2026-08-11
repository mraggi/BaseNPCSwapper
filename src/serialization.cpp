#include "serialization.hpp"
#include "NPCManager.hpp"
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BNS::Serialization
{

// Record type 'BNSA'.
//   V1: count, formID[].
//   V2: magic('BNSA'), count, formID[].
//   V3: magic, ruleSetFingerprint, sessionCount, sessionRefID[], nextTick,
//       processedCount, [refID, lastTouchedTick, persistent, matchedCount,
//       matchedRules[]] × processedCount.
// Save writes V3. Load supports V1/V2/V3.
constexpr std::uint32_t BNSF4_RECORD_VERSION_CURRENT = 3;
constexpr std::uint32_t BNSF4_RECORD_MAGIC = 'BNSA';

// Sanity bounds — corrupted records claim absurd counts.
constexpr std::uint32_t BNSF4_MAX_REASONABLE_COUNT = 300000;
constexpr std::uint32_t BNSF4_MAX_REASONABLE_RULES_PER_ACTOR = 4096;

// Migrated-from-V1/V2 sentinel: an actor carrying this hash skips every rule
// (preserves the old "fully processed against the unknown prior rule set"
// semantics). Drains naturally because lastTouchedTick = 0 sorts it first.
constexpr std::uint64_t kMigratedSentinel = 0xFFFFFFFFFFFFFFFFULL;

void SaveCallback(const F4SE::SerializationInterface* a_intfc)
{
    if (!a_intfc)
    {
        spdlog::error("[Serialize] Save: null interface.");
        return;
    }

    auto* manager = NPCManager::GetSingleton();
    auto snap = manager->GetSnapshot();
    const std::uint64_t fingerprint = manager->GetRuleSetFingerprint();

    // Filter 0xFF refIDs from both sets (engine's uniquified namespace; might
    // be recycled across sessions, persisting them risks false-negative skip).
    std::vector<std::uint32_t> persistentSession;
    persistentSession.reserve(snap.sessionActors.size());
    for (auto formID : snap.sessionActors)
    {
        if ((formID >> 24) != 0xFF) persistentSession.push_back(formID);
    }

    std::vector<std::uint32_t> persistentProcessed;
    persistentProcessed.reserve(snap.processedActors.size());
    for (const auto& [formID, _] : snap.processedActors)
    {
        if ((formID >> 24) != 0xFF) persistentProcessed.push_back(formID);
    }

    if (!a_intfc->OpenRecord(BNSF4_RECORD_ACTR, BNSF4_RECORD_VERSION_CURRENT))
    {
        spdlog::error("[Serialize] Save: OpenRecord failed.");
        return;
    }

    auto writeOrReturn = [&](const auto& a_val, const char* a_what) {
        if (!a_intfc->WriteRecordData(a_val))
        {
            spdlog::error("[Serialize] Save: WriteRecordData({}) failed.", a_what);
            return false;
        }
        return true;
    };

    if (!writeOrReturn(BNSF4_RECORD_MAGIC, "magic")) return;
    if (!writeOrReturn(fingerprint, "fingerprint")) return;

    const std::uint32_t sessionCount = static_cast<std::uint32_t>(persistentSession.size());
    if (!writeOrReturn(sessionCount, "sessionCount")) return;
    for (auto formID : persistentSession)
    {
        if (!writeOrReturn(formID, "session refID")) return;
    }

    if (!writeOrReturn(snap.nextTick, "nextTick")) return;

    const std::uint32_t processedCount = static_cast<std::uint32_t>(persistentProcessed.size());
    if (!writeOrReturn(processedCount, "processedCount")) return;
    for (auto formID : persistentProcessed)
    {
        const auto& e = snap.processedActors.at(formID);
        if (!writeOrReturn(formID, "processed refID")) return;
        if (!writeOrReturn(e.lastTouchedTick, "lastTouchedTick")) return;
        if (!writeOrReturn(static_cast<std::uint8_t>(e.persistent ? 1 : 0), "persistent")) return;
        const std::uint32_t matchedCount = static_cast<std::uint32_t>(e.matchedRules.size());
        if (!writeOrReturn(matchedCount, "matchedCount")) return;
        for (auto h : e.matchedRules)
        {
            if (!writeOrReturn(h, "matched rule hash")) return;
        }
    }

    spdlog::info("[Save] Wrote BNS state to save file: {} actor(s) already-processed this session, "
                 "{} actor(s) with per-rule history. Rule-set fingerprint {:016X}.",
                 sessionCount,
                 processedCount,
                 fingerprint);
}

namespace
{

    // V1/V2 helper: read count + flat refID list; populate the snapshot's
    // processed map with sentinel entries (mimics "fully processed for
    // unknown prior rule set"). Session set stays empty.
    bool ReadFlatRefIDsAsSentinel(const F4SE::SerializationInterface* a_intfc,
                                  NPCManager::Snapshot& a_out,
                                  const char* a_versionTag)
    {
        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count))
        {
            spdlog::error("[Serialize] Load {}: failed to read count.", a_versionTag);
            return false;
        }
        if (count > BNSF4_MAX_REASONABLE_COUNT)
        {
            spdlog::error("[Serialize] Load {}: count {} exceeds sanity bound {}. Refusing to load.",
                          a_versionTag,
                          count,
                          BNSF4_MAX_REASONABLE_COUNT);
            return false;
        }

        std::size_t resolved = 0;
        std::size_t skipped = 0;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            std::uint32_t oldFormID = 0;
            if (!a_intfc->ReadRecordData(oldFormID))
            {
                spdlog::error("[Serialize] Load {}: truncated at refID {}/{}.", a_versionTag, i, count);
                return false;
            }
            if (auto newFormID = a_intfc->ResolveFormID(oldFormID))
            {
                ProcessedEntry e;
                e.matchedRules.insert(kMigratedSentinel);
                e.lastTouchedTick = 0;
                e.persistent = false;
                a_out.processedActors.emplace(*newFormID, std::move(e));
                ++resolved;
            }
            else
            {
                ++skipped;
            }
        }
        a_out.nextTick = 1;

        if (skipped > 0)
        {
            spdlog::info("[Load] Old-format save ({}): {} of {} stored actors couldn't be looked up "
                         "(probably a plugin was removed from the load order since the save).",
                         a_versionTag,
                         skipped,
                         count);
        }
        spdlog::info("[Load] Old-format save ({}): restored {} actor(s) as 'already processed'. "
                     "These will be forgotten as the cache fills with new entries.",
                     a_versionTag,
                     resolved);
        return true;
    }

    bool LoadRecord_V1(const F4SE::SerializationInterface* a_intfc, NPCManager::Snapshot& a_out)
    { return ReadFlatRefIDsAsSentinel(a_intfc, a_out, "V1"); }

    bool LoadRecord_V2(const F4SE::SerializationInterface* a_intfc, NPCManager::Snapshot& a_out)
    {
        std::uint32_t magic = 0;
        if (!a_intfc->ReadRecordData(magic))
        {
            spdlog::error("[Serialize] Load V2: failed to read magic.");
            return false;
        }
        if (magic != BNSF4_RECORD_MAGIC)
        {
            spdlog::error("[Serialize] Load V2: magic mismatch (got {:08X}, want {:08X}). Aborting record.",
                          magic,
                          BNSF4_RECORD_MAGIC);
            return false;
        }
        return ReadFlatRefIDsAsSentinel(a_intfc, a_out, "V2");
    }

    bool LoadRecord_V3(const F4SE::SerializationInterface* a_intfc,
                       NPCManager::Snapshot& a_out,
                       std::uint64_t a_currentFingerprint)
    {
        std::uint32_t magic = 0;
        if (!a_intfc->ReadRecordData(magic))
        {
            spdlog::error("[Serialize] Load V3: failed to read magic.");
            return false;
        }
        if (magic != BNSF4_RECORD_MAGIC)
        {
            spdlog::error("[Serialize] Load V3: magic mismatch (got {:08X}). Aborting record.", magic);
            return false;
        }

        std::uint64_t savedFingerprint = 0;
        if (!a_intfc->ReadRecordData(savedFingerprint))
        {
            spdlog::error("[Serialize] Load V3: failed to read fingerprint.");
            return false;
        }
        const bool fingerprintMatches = (savedFingerprint == a_currentFingerprint);

        // Session set: read bytes either way (stream must advance), but
        // discard if fingerprint mismatched.
        std::uint32_t sessionCount = 0;
        if (!a_intfc->ReadRecordData(sessionCount))
        {
            spdlog::error("[Serialize] Load V3: failed to read sessionCount.");
            return false;
        }
        if (sessionCount > BNSF4_MAX_REASONABLE_COUNT)
        {
            spdlog::error("[Serialize] Load V3: sessionCount {} exceeds sanity bound. Aborting.", sessionCount);
            return false;
        }
        std::size_t sessionSkipped = 0;
        for (std::uint32_t i = 0; i < sessionCount; ++i)
        {
            std::uint32_t oldFormID = 0;
            if (!a_intfc->ReadRecordData(oldFormID))
            {
                spdlog::error("[Serialize] Load V3: truncated at session refID {}/{}.", i, sessionCount);
                return false;
            }
            if (!fingerprintMatches) continue; // discard, but still consumed the bytes
            if (auto newFormID = a_intfc->ResolveFormID(oldFormID)) { a_out.sessionActors.insert(*newFormID); }
            else
            {
                ++sessionSkipped;
            }
        }

        if (!a_intfc->ReadRecordData(a_out.nextTick))
        {
            spdlog::error("[Serialize] Load V3: failed to read nextTick.");
            return false;
        }

        std::uint32_t processedCount = 0;
        if (!a_intfc->ReadRecordData(processedCount))
        {
            spdlog::error("[Serialize] Load V3: failed to read processedCount.");
            return false;
        }
        if (processedCount > BNSF4_MAX_REASONABLE_COUNT)
        {
            spdlog::error("[Serialize] Load V3: processedCount {} exceeds sanity bound. Aborting.", processedCount);
            return false;
        }
        std::size_t processedSkipped = 0;
        for (std::uint32_t i = 0; i < processedCount; ++i)
        {
            std::uint32_t oldFormID = 0;
            std::uint64_t lastTouchedTick = 0;
            std::uint8_t persistent = 0;
            std::uint32_t matchedCount = 0;
            if (!a_intfc->ReadRecordData(oldFormID) || !a_intfc->ReadRecordData(lastTouchedTick)
                || !a_intfc->ReadRecordData(persistent) || !a_intfc->ReadRecordData(matchedCount))
            {
                spdlog::error("[Serialize] Load V3: truncated reading processed entry {}/{}.", i, processedCount);
                return false;
            }
            if (matchedCount > BNSF4_MAX_REASONABLE_RULES_PER_ACTOR)
            {
                spdlog::error("[Serialize] Load V3: matchedCount {} exceeds sanity bound. Aborting.", matchedCount);
                return false;
            }
            ProcessedEntry e;
            e.lastTouchedTick = lastTouchedTick;
            e.persistent = (persistent != 0);
            e.matchedRules.reserve(matchedCount);
            for (std::uint32_t j = 0; j < matchedCount; ++j)
            {
                std::uint64_t hash = 0;
                if (!a_intfc->ReadRecordData(hash))
                {
                    spdlog::error("[Serialize] Load V3: truncated reading matched rule {}/{} for entry {}.",
                                  j,
                                  matchedCount,
                                  i);
                    return false;
                }
                e.matchedRules.insert(hash);
            }
            if (auto newFormID = a_intfc->ResolveFormID(oldFormID))
            {
                a_out.processedActors.emplace(*newFormID, std::move(e));
            }
            else
            {
                ++processedSkipped;
            }
        }

        if (fingerprintMatches)
        {
            spdlog::info("[Load] Rules unchanged since save: restored {} 'already-processed' actor(s) "
                         "(skipped {} whose plugin is no longer loaded). Per-rule history kept for {} actor(s) "
                         "(skipped {} whose plugin is no longer loaded).",
                         a_out.sessionActors.size(),
                         sessionSkipped,
                         a_out.processedActors.size(),
                         processedSkipped);
        }
        else
        {
            spdlog::info("[Load] Rules have changed since save (added / removed / edited / reordered). "
                         "The 'already-processed this session' list was discarded — every actor will be "
                         "re-evaluated next time they load. Per-rule history kept for {} actor(s) "
                         "(skipped {} whose plugin is no longer loaded), used to skip rules that have "
                         "already fired on each actor in past sessions.",
                         a_out.processedActors.size(),
                         processedSkipped);
        }
        spdlog::debug("[Load] (Fingerprint debug: saved={:016X}, current={:016X}.)",
                      savedFingerprint,
                      a_currentFingerprint);
        return true;
    }

} // namespace

// Unknown record types and versions are skipped with a warning — never
// re-interpreted as a known version.
void LoadCallback(const F4SE::SerializationInterface* a_intfc)
{
    if (!a_intfc)
    {
        spdlog::error("[Serialize] Load: null interface.");
        return;
    }

    auto* manager = NPCManager::GetSingleton();
    NPCManager::Snapshot snap;

    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    bool bail = false;

    while (!bail && a_intfc->GetNextRecordInfo(type, version, length))
    {
        if (type != BNSF4_RECORD_ACTR)
        {
            spdlog::warn("[Serialize] Load: unknown record type {:08X} (len {}). Skipping.", type, length);
            continue;
        }

        switch (version)
        {
        case 1:
            if (!LoadRecord_V1(a_intfc, snap)) bail = true;
            break;
        case 2:
            if (!LoadRecord_V2(a_intfc, snap)) bail = true;
            break;
        case 3:
            if (!LoadRecord_V3(a_intfc, snap, manager->GetRuleSetFingerprint())) bail = true;
            break;
        default:
            // Future versions add a case here. Never fall through.
            spdlog::warn("[Serialize] Load: unknown record version {} for type BNSA (len {}). Skipping. "
                         "This save was probably written by a newer plugin version.",
                         version,
                         length);
            break;
        }
    }

    if (bail)
    {
        spdlog::error("[Serialize] Load: record parse failed; discarding partial snapshot.");
        manager->SetSnapshot({});
    }
    else
    {
        manager->SetSnapshot(std::move(snap));
        manager->EvictOldestIfNeeded();
    }
}

// New game / main menu: throw away all session state.
void RevertCallback(const F4SE::SerializationInterface*)
{
    NPCManager::GetSingleton()->SetSnapshot({});
    spdlog::info("[Revert] Returning to main menu / loading a new game — cleared all per-actor BNS state.");
}

} // namespace BNS::Serialization
