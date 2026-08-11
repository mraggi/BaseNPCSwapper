#include "NPCModifier.hpp"
#include "DelayManager.hpp"
#include "MNAMResolver.hpp"
#include "NPCManager.hpp"
#include "SwapRuleResolved.hpp"
#include "Utils.hpp"
#include <F4SE/F4SE.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <mutex>
#include <random>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BNS::Modifier
{
// Drain delays let Papyrus finish drop/attach/add/equip before the next
// rule's dispatch lands on the same actor. Bumping needed if symptoms
// appear: OMODs visible in inventory but item ends up unequipped.
constexpr int kModifyNoOMODsDelayMS = 32;
constexpr int kModifyBaseDelayMS = 256;
constexpr int kModifyPerBatchDelayMS = 128;
constexpr int kEquipBatchTimeoutMS = 2000;

// MNAMResolver must have been built before calling this.
OMODDictionary BuildOMODCache(const std::vector<RE::BGSMod::Attachment::Mod*>& a_omods)
{
    OMODDictionary cache;
    if (a_omods.empty()) return cache;

    for (auto* omod : a_omods)
    {
        if (!omod) continue;

        const auto apIndex = omod->attachPoint.keywordIndex;
        auto* mnam = BNS::MNAMResolver::GetMNAM(omod);

        std::vector<RE::BGSKeyword*> reqs;
        if (mnam) reqs.push_back(mnam);

        CachedOMOD entry {omod, reqs};
        cache.byAttachPoint[apIndex].push_back(entry);

        // Second index keyed by MNAM kw → ma_* keyword for PA items.
        if (mnam)
        {
            const auto mnamIdx = RE::detail::BGSKeywordGetIndexForTypedKeyword(mnam, RE::KeywordType::kModAssociation);
            cache.byMNAM[mnamIdx].push_back(entry);
        }
    }

    return cache;
}

namespace
{

    // Max OMODs per SafeAttachMods Papyrus call. Must match the signature
    // in BNSSpawnHelper.psc.
    constexpr std::size_t kOMODBatchSize = 8;

    bool IsItemEquipped(const RE::BGSInventoryItem& a_item)
    {
        auto currentStack = a_item.stackData;
        while (currentStack)
        {
            if (currentStack->IsEquipped()) return true;
            currentStack = currentStack->nextStack;
        }
        return false;
    }

    // Thread-local RNG — pick is called once per slot per item per actor.
    // ApplyOMODs re-seeds it from (refActor ^ rule.hash) at the top of each
    // run so OMOD picks are deterministic per (actor, rule) — match chance
    // is already seeded the same way in NPCEvaluator::RollChanceSeeded, so
    // re-evaluating the same actor against the same rule reproduces the
    // exact same OMOD layout. mt19937_64 matches the 64-bit seed shape used
    // by RollChanceSeeded. OMOD work always runs on the main thread (via
    // task->AddTask), so the thread_local is safe.
    //
    // Note: this determinism covers the *evaluator* (chance) and OMOD picks
    // only. The swap target itself is NOT seeded this way — when replaceBy is
    // an LVLN, Swapper::RollLVLN rolls fresh each time on purpose, so adding a
    // leveled character behaves like the engine's own leveled-list randomness.
    std::mt19937_64& PickerRNG()
    {
        thread_local std::mt19937_64 rng {std::random_device {}()};
        return rng;
    }

    // Pick one OMOD. a_chosenCache=nullptr → uniform random (independent
    // picks). a_chosenCache non-null → mutated per-actor "already chosen"
    // set; prefer picks that overlap the cache so picks stay consistent
    // across slots that share candidates (e.g. one receiver mod for all
    // guns of a variant). PA paint OMODs per-piece don't overlap and need
    // authoring to share parents.
    RE::BGSMod::Attachment::Mod* PickRandomOMOD(const std::vector<CachedOMOD>& a_candidates,
                                                std::vector<RE::BGSMod::Attachment::Mod*>* a_chosenCache)
    {
        if (a_candidates.empty()) return nullptr;

        auto& rng = PickerRNG();

        // No consistency requested → roll fresh each time.
        if (!a_chosenCache)
        {
            std::uniform_int_distribution<std::size_t> dist(0, a_candidates.size() - 1);
            return a_candidates[dist(rng)].mod;
        }

        // Consistency mode: prefer a previously-picked candidate. The
        // intersection is almost always 0 or 1 — linear scan is cheap.
        std::vector<RE::BGSMod::Attachment::Mod*> intersection;
        intersection.reserve(a_chosenCache->size());
        for (const auto& cand : a_candidates)
        {
            if (!cand.mod) continue;
            if (std::find(a_chosenCache->begin(), a_chosenCache->end(), cand.mod) != a_chosenCache->end())
            {
                intersection.push_back(cand.mod);
            }
        }

        if (!intersection.empty())
        {
            std::uniform_int_distribution<std::size_t> dist(0, intersection.size() - 1);
            return intersection[dist(rng)];
        }

        // Nothing in the cache matches → pick fresh and record it so the
        // next overlapping slot reuses it.
        std::uniform_int_distribution<std::size_t> dist(0, a_candidates.size() - 1);
        auto* picked = a_candidates[dist(rng)].mod;
        if (picked) a_chosenCache->push_back(picked);
        return picked;
    }

    // AttachModsToItem does drop+attach+re-add only; it then calls back via
    // BNSOnAttachComplete so C++ can queue the (actor, item) pair for
    // re-equip *after* the drain. Equipping in the same VM turn as the drop
    // races with the engine's outfit reconciliation — the equip succeeds
    // but reconciliation un-equips it on the next tick.
    void DispatchPapyrusAttachBatch(RE::Actor* a_actor,
                                    RE::TESBoundObject* a_baseObj,
                                    const std::vector<RE::BGSMod::Attachment::Mod*>& a_omods,
                                    bool a_isEquipped,
                                    int a_debugLevel)
    {
        if (a_omods.empty()) return;

        std::array<std::uint32_t, kOMODBatchSize> formIDs {};
        for (std::size_t i = 0; i < a_omods.size() && i < kOMODBatchSize; ++i)
        {
            formIDs[i] = a_omods[i] ? a_omods[i]->GetFormID() : 0;
        }

        if (a_debugLevel >= 4)
        {
            spdlog::debug("[Timing] [{:08X}] -> dispatching AttachModsToItem({} OMODs, wasEquipped={}) on '{}' "
                          "[{:08X}]",
                          a_actor->GetFormID(),
                          a_omods.size(),
                          a_isEquipped,
                          Utils::GetFormName(a_baseObj),
                          a_baseObj->GetFormID());
        }

        Utils::DispatchToPapyrus("BNSSpawnHelper",
                                 "AttachModsToItem",
                                 a_actor->GetFormID(),
                                 a_baseObj->GetFormID(),
                                 formIDs[0],
                                 formIDs[1],
                                 formIDs[2],
                                 formIDs[3],
                                 formIDs[4],
                                 formIDs[5],
                                 formIDs[6],
                                 formIDs[7],
                                 a_isEquipped);
    }

    // Per-actor re-equip queue, filled by OnAttachComplete callbacks during
    // the attach phase and drained by the post-drain logic. Holding it in a
    // mutex-protected map keeps it independent of the modification task's
    // local state (which is already gone by the time the callbacks arrive).
    std::mutex g_pendingEquipsMutex;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> g_pendingEquips;

    void RegisterPendingEquip(std::uint32_t a_refActor, std::uint32_t a_refItem)
    {
        std::lock_guard<std::mutex> lk(g_pendingEquipsMutex);
        g_pendingEquips[a_refActor].push_back(a_refItem);
    }

    std::vector<std::uint32_t> ConsumePendingEquips(std::uint32_t a_refActor)
    {
        std::lock_guard<std::mutex> lk(g_pendingEquipsMutex);
        auto it = g_pendingEquips.find(a_refActor);
        if (it == g_pendingEquips.end()) return {};
        auto items = std::move(it->second);
        g_pendingEquips.erase(it);
        return items;
    }

    // Equip-phase in-flight batch. One entry per actor while the equip
    // dispatches are outstanding. OnEquipComplete decrements; reaching zero
    // (or the timeout below) fires onAllDone exactly once.
    struct EquipBatch
    {
        std::size_t remaining = 0;
        std::function<void()> onAllDone;
        int debugLevel = 0;
    };

    std::mutex g_equipBatchMutex;
    std::unordered_map<std::uint32_t, EquipBatch> g_equipBatches;

    void StartEquipBatch(std::uint32_t a_refActor, std::size_t a_count, int a_debugLevel, std::function<void()> a_cb)
    {
        std::lock_guard<std::mutex> lk(g_equipBatchMutex);
        g_equipBatches[a_refActor] = EquipBatch {a_count, std::move(a_cb), a_debugLevel};
    }

    // Returns the completion callback iff this decrement reaches zero.
    // Atomically removes the batch entry so the timeout can't double-fire.
    std::function<void()> DecrementEquipBatch(std::uint32_t a_refActor, std::size_t& a_outRemaining)
    {
        std::lock_guard<std::mutex> lk(g_equipBatchMutex);
        auto it = g_equipBatches.find(a_refActor);
        if (it == g_equipBatches.end())
        {
            a_outRemaining = 0;
            return {};
        }
        if (it->second.remaining > 0) --it->second.remaining;
        a_outRemaining = it->second.remaining;
        if (it->second.remaining == 0)
        {
            auto cb = std::move(it->second.onAllDone);
            g_equipBatches.erase(it);
            return cb;
        }
        return {};
    }

    // Force-fire the callback regardless of remaining count, used by the
    // timeout. Idempotent — if the batch is already gone, returns empty.
    std::function<void()> ForceClearEquipBatch(std::uint32_t a_refActor, std::size_t& a_outRemaining)
    {
        std::lock_guard<std::mutex> lk(g_equipBatchMutex);
        auto it = g_equipBatches.find(a_refActor);
        if (it == g_equipBatches.end())
        {
            a_outRemaining = 0;
            return {};
        }
        a_outRemaining = it->second.remaining;
        auto cb = std::move(it->second.onAllDone);
        g_equipBatches.erase(it);
        return cb;
    }

    // Keep only OMODs whose ma_* recipe keywords match the item (or that
    // have no ma_* gate at all).
    std::vector<CachedOMOD> GetValidCandidates(const std::vector<CachedOMOD>& a_candidates,
                                               RE::BGSKeywordForm* a_itemKwForm)
    {
        std::vector<CachedOMOD> validCandidates;
        if (!a_itemKwForm) return validCandidates;

        for (const auto& candidate : a_candidates)
        {
            if (candidate.maKeywords.empty())
            {
                validCandidates.push_back(candidate);
                continue;
            }
            const bool matches = std::ranges::any_of(candidate.maKeywords, [a_itemKwForm](RE::BGSKeyword* a_kw) {
                return a_kw && a_itemKwForm->HasKeyword(a_kw);
            });
            if (matches) validCandidates.push_back(candidate);
        }
        return validCandidates;
    }

    RE::BGSAttachParentArray* GetItemAttachParents(RE::TESBoundObject* a_baseObj)
    {
        if (!a_baseObj) return nullptr;
        if (a_baseObj->Is(RE::ENUM_FORM_ID::kWEAP)) { return &a_baseObj->As<RE::TESObjectWEAP>()->attachParents; }
        if (a_baseObj->Is(RE::ENUM_FORM_ID::kARMO)) { return &a_baseObj->As<RE::TESObjectARMO>()->attachParents; }
        return nullptr;
    }

    // Every pick goes through Papyrus SafeAttachMods, which uses the OMOD's
    // own attachPoint (not an array index). See OldCrap.hpp for the prior
    // in-place routing that needed a slot index.
    struct ItemModPick
    {
        RE::BGSMod::Attachment::Mod* omod;
    };

    // Plan OMODs for one item via two paths (de-duped):
    //   AP   — iterate item APPR, look up byAttachPoint (weapons/regular armor).
    //   MNAM — iterate item keywords, look up byMNAM via kModAssociation
    //          (PA pieces, anything ma_*-matched).
    // Pure planning — does NOT touch inventory or fire Papyrus.
    std::vector<ItemModPick> PlanItemOMODs(RE::BGSInventoryItem& a_item,
                                           const OMODDictionary& a_omodCache,
                                           std::vector<RE::BGSMod::Attachment::Mod*>* a_chosenCache,
                                           int a_debugLevel)
    {
        std::vector<ItemModPick> picks;

        RE::TESBoundObject* baseObj = a_item.object;
        if (!baseObj) return picks;

        auto* keywordForm = baseObj->As<RE::BGSKeywordForm>();
        if (!keywordForm) return picks;

        // Null for non-WEAP/ARMO; that disables the AP path only, MNAM still runs.
        auto* itemAttachParents = GetItemAttachParents(baseObj);

        // Planning diagnostics are level 3 (why an item didn't match).
        // Level 2 only shows the per-item summary (one line: name -> OMODs).
        const bool diag = a_debugLevel >= 3;
        std::unordered_set<RE::BGSMod::Attachment::Mod*> alreadyPicked;
        std::size_t apCacheHits = 0, apCandidateHits = 0;
        std::size_t mnamCacheHits = 0;

        // ---- Path 1: attach-parent (weapons / regular armor) ----
        if (itemAttachParents)
        {
            for (std::uint32_t i = 0; i < itemAttachParents->size; ++i)
            {
                const std::uint16_t apIndex = itemAttachParents->array[i].keywordIndex;
                auto cacheIt = a_omodCache.byAttachPoint.find(apIndex);
                if (cacheIt == a_omodCache.byAttachPoint.end())
                {
                    if (diag)
                    {
                        spdlog::debug("        no OMOD in this rule targets attach-point #{} of '{}'",
                                      i,
                                      Utils::GetFormName(baseObj));
                    }
                    continue;
                }
                ++apCacheHits;

                auto validCandidates = GetValidCandidates(cacheIt->second, keywordForm);
                if (validCandidates.empty())
                {
                    if (diag)
                    {
                        spdlog::debug("        {} OMOD(s) target attach-point #{} of '{}', "
                                      "but none of them match the item's recipe keywords",
                                      cacheIt->second.size(),
                                      i,
                                      Utils::GetFormName(baseObj));
                    }
                    continue;
                }
                ++apCandidateHits;

                auto* selected = PickRandomOMOD(validCandidates, a_chosenCache);
                if (selected && alreadyPicked.insert(selected).second) { picks.push_back({selected}); }
            }
        }

        // ---- Path 2: MNAM (PA pieces / anything matched by ma_* keyword) ----
        for (std::uint32_t k = 0; k < keywordForm->numKeywords; ++k)
        {
            auto* kw = keywordForm->keywords[k];
            if (!kw) continue;

            const std::uint16_t mnamIdx
              = RE::detail::BGSKeywordGetIndexForTypedKeyword(kw, RE::KeywordType::kModAssociation);
            if (mnamIdx == 0xFFFF) continue; // not a ma_* keyword

            auto cacheIt = a_omodCache.byMNAM.find(mnamIdx);
            if (cacheIt == a_omodCache.byMNAM.end()) continue;
            ++mnamCacheHits;

            // Every OMOD in this bucket already MNAM-matches this keyword.
            auto* selected = PickRandomOMOD(cacheIt->second, a_chosenCache);
            if (selected && alreadyPicked.insert(selected).second)
            {
                if (diag)
                {
                    spdlog::debug("        matched '{}' on item keyword '{}' (picked from {} candidate(s))",
                                  Utils::GetFormName(selected),
                                  Utils::GetFormName(kw),
                                  cacheIt->second.size());
                }
                picks.push_back({selected});
            }
        }

        if (diag && picks.empty() && (itemAttachParents && itemAttachParents->size > 0))
        {
            spdlog::debug("        no OMODs matched '{}': {} attach-point(s) checked, "
                          "{} had cache entries but 0 passed the recipe-keyword filter",
                          Utils::GetFormName(baseObj),
                          itemAttachParents->size,
                          apCacheHits);
        }

        return picks;
    }

    void ApplyFactions(RE::Actor* a_actor, const SwapRuleResolved& a_rule)
    {
        if (a_rule.addFactions.empty()) return;

        if (!a_actor->extraList) a_actor->extraList.reset(new RE::ExtraDataList());

        auto* extraFac = a_actor->extraList->GetByType<RE::ExtraFactionChanges>();
        if (!extraFac)
        {
            extraFac = new RE::ExtraFactionChanges();
            a_actor->extraList->AddExtra(extraFac);
        }

        for (auto* faction : a_rule.addFactions)
        {
            if (!faction) continue;

            const bool exists = std::ranges::any_of(extraFac->factionChanges, [faction](const auto& a_existing) {
                return a_existing.faction == faction;
            });

            if (exists) continue;

            RE::FACTION_RANK rank;
            rank.faction = faction;
            rank.rank = 1;
            extraFac->factionChanges.push_back(rank);
        }
    }

    // Resolve one addItems entry: if it's an LVLI, roll it against the
    // player's level and add the rolled leaves; otherwise add directly.
    void GiveOneAddItem(RE::Actor* a_actor, RE::TESBoundObject* a_item)
    {
        if (!a_item) return;
        if (a_item->formType != RE::ENUM_FORM_ID::kLVLI)
        {
            a_actor->AddObjectToContainer(a_item, {}, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
            return;
        }
        auto* lvli = a_item->As<RE::TESLevItem>();
        if (!lvli) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const std::uint16_t level = player ? player->GetLevel() : 1;
        RE::BSScrapArray<RE::CALCED_OBJECT> calcOut;
        lvli->CalculateCurrentFormList(level, 1, calcOut, RE::TESLeveledList::LeveledListAllBelowForce::kDefault, false);
        for (const auto& entry : calcOut)
        {
            if (!entry.object) continue;
            const auto count = std::max<std::int32_t>(1, entry.count);
            a_actor->AddObjectToContainer(entry.object, {}, count, nullptr, RE::ITEM_REMOVE_REASON::kNone);
        }
    }

    void ApplyItems(RE::Actor* a_actor, const SwapRuleResolved& a_rule)
    {
        for (auto* item : a_rule.addItems)
        {
            GiveOneAddItem(a_actor, item);
        }
    }

    // Raise the actor's base SPECIAL toward a level-scaled target total,
    // distributing the shortfall randomly across the 7 stats. Floor-to-target:
    // never lowers a stat, and re-running once the target is met is a no-op
    // (idempotent). Deterministic per (actor, rule) via the shared seed.
    void ApplySpecial(RE::Actor* a_actor, const SwapRuleResolved& a_rule)
    {
        if (!a_rule.boostSpecial) return;

        auto* av = RE::ActorValue::GetSingleton();
        if (!av) return;

        const std::array<RE::ActorValueInfo*, 7> stats
          = {av->strength, av->perception, av->endurance, av->charisma, av->intelligence, av->agility, av->luck};
        for (auto* s : stats)
            if (!s) return; // SPECIAL forms missing — nothing safe to do

        const int level = a_actor->GetLevel();

        float targetF = a_rule.specialBaseline + a_rule.specialPerLevel * static_cast<float>(level);
        targetF = std::clamp(targetF, a_rule.specialBaseline, a_rule.specialMaxTotal);
        const int target = static_cast<int>(std::lround(targetF));
        const int cap = a_rule.specialPerStatCap;

        std::array<int, 7> vals {};
        int current = 0;
        for (std::size_t i = 0; i < stats.size(); ++i)
        {
            vals[i] = static_cast<int>(std::lround(a_actor->GetBaseActorValue(*stats[i])));
            current += vals[i];
        }

        int deficit = target - current;
        if (deficit <= 0)
        {
            if (a_rule.debugLevel >= 2)
                spdlog::debug("    [SPECIAL] [{:08X}] lvl {} already at {} total (>= target {}) — no change",
                              a_actor->GetFormID(),
                              level,
                              current,
                              target);
            return;
        }

        if (auto* npc = a_actor->GetNPC(); npc && npc->HasAutoCalcStats() && a_rule.debugLevel >= 1)
            spdlog::warn("    [SPECIAL] [{:08X}] is auto-calc — base SPECIAL write may be re-derived by the engine",
                         a_actor->GetFormID());

        // Deterministic per (actor, rule) — same seed convention as the OMOD picker.
        std::mt19937_64 rng(static_cast<std::uint64_t>(a_actor->GetFormID()) ^ a_rule.hash);
        std::uniform_int_distribution<std::size_t> pick(0, stats.size() - 1);

        while (deficit > 0)
        {
            if (!std::ranges::any_of(vals, [cap](int v) { return v < cap; })) break; // all capped

            const std::size_t idx = pick(rng);
            if (vals[idx] >= cap) continue; // rejection-sample onto an eligible stat
            ++vals[idx];
            --deficit;
        }

        for (std::size_t i = 0; i < stats.size(); ++i)
            a_actor->SetBaseActorValue(*stats[i], static_cast<float>(vals[i]));

        if (a_rule.debugLevel >= 1)
            spdlog::info("    [SPECIAL] [{:08X}] lvl {}: {} -> {} total  (S{} P{} E{} C{} I{} A{} L{})",
                         a_actor->GetFormID(),
                         level,
                         current,
                         target - deficit,
                         vals[0],
                         vals[1],
                         vals[2],
                         vals[3],
                         vals[4],
                         vals[5],
                         vals[6]);
    }

    // Every pick is dispatched via Papyrus SafeAttachMods — see OldCrap.hpp
    // for the in-place attach path that used to handle PA pieces; it was
    // implicated in a rare crash, so we route everything the same way now.
    struct OMODApplyResult
    {
        std::size_t papyrusBatches = 0;
        std::size_t papyrusAttached = 0;
    };

    OMODApplyResult ApplyOMODs(RE::Actor* a_actor, const SwapRuleResolved& a_rule)
    {
        OMODApplyResult result;
        if (a_rule.addOMODs.empty()) return result;

        const auto& omodCache = a_rule.OMODCache;
        if (omodCache.empty()) return result;

        // Determinism: same actor + same rule = same OMOD layout. Match
        // chance is seeded the same way in NPCEvaluator::RollChanceSeeded.
        const std::uint64_t pickSeed = static_cast<std::uint64_t>(a_actor->GetFormID()) ^ a_rule.hash;
        PickerRNG().seed(pickSeed);

        // OMODRandomizationPerItem: shared cache so picks are consistent
        // across an actor's inventory (e.g. same barrel mod on all weapons
        // that can take it). nullptr → each pick independent.
        std::vector<RE::BGSMod::Attachment::Mod*> chosenCache;
        std::vector<RE::BGSMod::Attachment::Mod*>* chosenCachePtr = a_rule.OMODRandomizationPerItem ? &chosenCache
                                                                                                    : nullptr;

        auto walkInventory = [&](RE::BGSInventoryList* a_inv, RE::Actor* a_equipTarget, const char* /*a_label*/) {
            if (!a_inv) return;

            std::size_t equippedCount = 0;
            for (const auto& it : a_inv->data)
                if (IsItemEquipped(it)) ++equippedCount;

            if (a_rule.debugLevel >= 4)
            {
                spdlog::debug("[Timing] [{:08X}] OMOD walk: {} item(s), {} currently equipped",
                              a_equipTarget ? a_equipTarget->GetFormID() : 0,
                              a_inv->data.size(),
                              equippedCount);
            }

            if (a_rule.debugLevel >= 2)
            {
                spdlog::debug("    Scanning inventory ({} item(s), {} equipped):", a_inv->data.size(), equippedCount);
            }

            for (auto& item : a_inv->data)
            {
                auto picks = PlanItemOMODs(item, omodCache, chosenCachePtr, a_rule.debugLevel);

                // Level 2: one row per item — name -> picked OMOD list, or "no OMODs matched".
                if (a_rule.debugLevel >= 2 && item.object)
                {
                    if (picks.empty())
                    {
                        spdlog::debug("      {} [{:08X}]  ->  no OMODs matched",
                                      Utils::GetFormName(item.object),
                                      item.object->GetFormID());
                    }
                    else
                    {
                        std::string names;
                        for (const auto& pick : picks)
                        {
                            if (!names.empty()) names += ", ";
                            names += Utils::GetFormName(pick.omod);
                        }
                        spdlog::debug("      {} [{:08X}]  ->  {}",
                                      Utils::GetFormName(item.object),
                                      item.object->GetFormID(),
                                      names);
                    }
                }

                if (picks.empty()) continue;

                RE::TESBoundObject* baseObj = item.object;
                if (!baseObj) continue;
                const bool isEquipped = IsItemEquipped(item);

                std::vector<RE::BGSMod::Attachment::Mod*> mods;
                mods.reserve(picks.size());
                for (const auto& pick : picks)
                    mods.push_back(pick.omod);

                for (std::size_t off = 0; off < mods.size(); off += kOMODBatchSize)
                {
                    const std::size_t end = std::min(off + kOMODBatchSize, mods.size());
                    std::vector<RE::BGSMod::Attachment::Mod*> chunk(mods.begin() + off, mods.begin() + end);
                    DispatchPapyrusAttachBatch(a_equipTarget, baseObj, chunk, isEquipped, a_rule.debugLevel);
                    ++result.papyrusBatches;
                    result.papyrusAttached += chunk.size();
                }
            }
        };

        walkInventory(a_actor->inventoryList, a_actor, "actor");

        return result;
    }

    OMODApplyResult ApplyAll(RE::Actor* a_actor, const SwapRuleResolved& a_rule)
    {
        if (!a_actor) return {};
        ApplyFactions(a_actor, a_rule);
        ApplyItems(a_actor, a_rule);
        ApplySpecial(a_actor, a_rule);
        return ApplyOMODs(a_actor, a_rule);
    }

    // =================================================================
    // ApplyModificationsAsync — three-phase modifier pipeline. The body
    // is split across the helpers below; entry point is at the bottom of
    // this file. Phases:
    //   1. cache gate (DeferIfOMODCacheNotReady)
    //   2. main-thread work (RunModifyMainTask -> ApplyAll)
    //   3. settle delay (DelayManager) then re-equip phase (RunReEquipPhase)
    // =================================================================

    // Forward-decl so the cache-gate's re-queue can call back into the
    // public entry point.
    void ApplyModificationsAsyncImpl(std::uint32_t a_refID,
                                     const SwapRuleResolved& a_rule,
                                     std::function<void()> a_onComplete);

    void LogModifyBegin(std::uint32_t a_refID, const SwapRuleResolved& a_rule)
    {
        if (a_rule.debugLevel < 1) return;
        auto* refForm = RE::TESForm::GetFormByID(a_refID);
        spdlog::debug("[{} | {}] Applying modifications to '{}' [{:08X}]",
                      a_rule.sourceFile,
                      a_rule.ruleName,
                      Utils::GetFormName(refForm),
                      a_refID);
    }

    // One-line summary of what actually happened. Called from both the
    // re-equip-complete callback and the no-equips-needed shortcut.
    void LogModifyDone(std::uint32_t a_refID,
                       const SwapRuleResolved& a_rule,
                       std::size_t a_attachedCount,
                       std::size_t a_reEquipped)
    {
        if (a_rule.debugLevel < 1) return;
        auto* refForm = RE::TESForm::GetFormByID(a_refID);
        const auto name = Utils::GetFormName(refForm);
        const auto factionCount = a_rule.addFactions.size();
        const auto givenCount = a_rule.addItems.size();

        if (a_attachedCount == 0 && factionCount == 0 && givenCount == 0)
        {
            spdlog::debug("[{} | {}] Done on '{}' [{:08X}]: no modifications applied "
                          "(nothing in the inventory matched any OMOD in this rule).",
                          a_rule.sourceFile,
                          a_rule.ruleName,
                          name,
                          a_refID);
            return;
        }

        spdlog::debug("[{} | {}] Done on '{}' [{:08X}]: "
                      "{} OMOD(s) attached, {} item(s) re-equipped{}{}.",
                      a_rule.sourceFile,
                      a_rule.ruleName,
                      name,
                      a_refID,
                      a_attachedCount,
                      a_reEquipped,
                      factionCount ? std::format(", {} faction(s) added", factionCount) : std::string {},
                      givenCount ? std::format(", {} item(s) given", givenCount) : std::string {});
    }

    // True if the call was deferred (OMOD cache not yet ready). Caller
    // must return immediately when true — onComplete has either been
    // re-queued or fired already.
    bool DeferIfOMODCacheNotReady(std::uint32_t a_refID, const SwapRuleResolved& a_rule, std::function<void()>& a_onComplete)
    {
        if (a_rule.addOMODs.empty() || MNAMResolver::IsCacheReady()) return false;

        if (NPCManager::GetSingleton()->IsShuttingDown())
        {
            spdlog::debug("[{} | {}] Shutting down before OMOD scan finished — "
                          "skipping OMODs for actor [{:08X}].",
                          a_rule.sourceFile,
                          a_rule.ruleName,
                          a_refID);
            if (a_onComplete) a_onComplete();
            return true;
        }

        if (a_rule.debugLevel >= 2)
        {
            spdlog::debug("[{} | {}] OMOD scan not finished yet — will re-check actor [{:08X}] in 256ms.",
                          a_rule.sourceFile,
                          a_rule.ruleName,
                          a_refID);
        }
        DelayManager::QueueTask(256, [a_refID, a_rule, cb = std::move(a_onComplete)]() mutable {
            ApplyModificationsAsyncImpl(a_refID, a_rule, std::move(cb));
        });
        return true;
    }

    // dev-only: snapshot of currently-equipped item count, mirrors the
    // helper in NPCSwapper. Used in [Timing] logs at level 4.
    std::size_t CountEquippedItems(RE::Actor* a_actor)
    {
        if (!a_actor || !a_actor->inventoryList) return 0;
        std::size_t n = 0;
        for (const auto& it : a_actor->inventoryList->data)
            if (IsItemEquipped(it)) ++n;
        return n;
    }

    // Timeout guard for the re-equip phase. If Papyrus never reports back
    // for some item, the per-actor EquipBatch would deadlock the pipeline;
    // this fires the completion callback anyway after kEquipBatchTimeoutMS.
    void ScheduleEquipBatchTimeout(std::uint32_t a_refID, const SwapRuleResolved& a_rule)
    {
        DelayManager::QueueTask(kEquipBatchTimeoutMS,
                                [a_refID, sourceFile = a_rule.sourceFile, ruleName = a_rule.ruleName]() {
                                    std::size_t remaining = 0;
                                    auto cb = ForceClearEquipBatch(a_refID, remaining);
                                    if (cb)
                                    {
                                        spdlog::warn("[{} | {}] Re-equip phase timed out for actor [{:08X}] "
                                                     "({} item(s) didn't report back) — moving on anyway.",
                                                     sourceFile,
                                                     ruleName,
                                                     a_refID,
                                                     remaining);
                                        cb();
                                    }
                                });
    }

    // Phase 3: re-equip everything that was equipped before AttachModsToItem
    // dropped it. Fires onComplete when every EquipItemSafe callback has
    // returned (or the timeout above kicks in).
    void RunReEquipPhase(std::uint32_t a_refID,
                         const SwapRuleResolved& a_rule,
                         std::size_t a_attachedCount,
                         std::function<void()> a_onComplete)
    {
        auto items = ConsumePendingEquips(a_refID);

        if (a_rule.debugLevel >= 4)
        {
            auto* f = RE::TESForm::GetFormByID(a_refID);
            auto* a = f ? f->As<RE::Actor>() : nullptr;
            spdlog::debug("[Timing] [{:08X}] Settle done; equipped now={}, re-equipping {} item(s)",
                          a_refID,
                          CountEquippedItems(a),
                          items.size());
        }

        if (items.empty())
        {
            LogModifyDone(a_refID, a_rule, a_attachedCount, 0);
            if (a_onComplete) a_onComplete();
            return;
        }

        const std::size_t equipCount = items.size();
        StartEquipBatch(a_refID,
                        equipCount,
                        a_rule.debugLevel,
                        [a_refID, a_rule, a_attachedCount, equipCount, cb = std::move(a_onComplete)]() mutable {
                            LogModifyDone(a_refID, a_rule, a_attachedCount, equipCount);
                            if (cb) cb();
                        });

        for (auto itemID : items)
        {
            Utils::DispatchToPapyrus("BNSSpawnHelper", "EquipItemSafe", a_refID, itemID);
        }

        ScheduleEquipBatchTimeout(a_refID, a_rule);
    }

    // Phase 2: the main-thread task body. Applies factions/items/OMODs
    // synchronously, then schedules a settle delay before the re-equip
    // phase. Fires onComplete early on vanished actors / no-op rules.
    void RunModifyMainTask(std::uint32_t a_refID, SwapRuleResolved a_rule, std::function<void()> a_onComplete)
    {
        auto* form = RE::TESForm::GetFormByID(a_refID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor)
        {
            if (a_rule.debugLevel >= 1)
            {
                spdlog::debug("[{} | {}] Actor [{:08X}] vanished before modifications could be applied.",
                              a_rule.sourceFile,
                              a_rule.ruleName,
                              a_refID);
            }
            if (a_onComplete) a_onComplete();
            return;
        }

        if (a_rule.debugLevel >= 4)
        {
            spdlog::debug("[Timing] [{:08X}] ApplyModificationsAsync task running -> ApplyAll starting", a_refID);
        }

        const auto res = ApplyAll(actor, a_rule);

        // Pacing: rules with no OMODs barely need any wait; OMOD rules
        // need the engine a moment to settle between drop+attach+re-add
        // and the re-equip phase.
        const int settleMS = (res.papyrusBatches == 0)
          ? kModifyNoOMODsDelayMS
          : (kModifyBaseDelayMS + static_cast<int>(res.papyrusBatches) * kModifyPerBatchDelayMS);

        if (a_rule.debugLevel >= 4)
        {
            spdlog::debug("[Timing] [{:08X}] {} attach request(s) dispatched -> "
                          "waiting {}ms for engine to settle, then re-equip phase",
                          a_refID,
                          res.papyrusBatches,
                          settleMS);
        }

        DelayManager::QueueTask(settleMS,
                                [a_refID,
                                 rule = std::move(a_rule),
                                 attachedCount = res.papyrusAttached,
                                 cb = std::move(a_onComplete)]() mutable {
                                    RunReEquipPhase(a_refID, rule, attachedCount, std::move(cb));
                                });
    }

    // Shared body for ApplyModificationsAsync — referenced by the cache
    // gate's re-queue. The public entry point at the bottom of the file
    // forwards here.
    void ApplyModificationsAsyncImpl(std::uint32_t a_refID,
                                     const SwapRuleResolved& a_rule,
                                     std::function<void()> a_onComplete)
    {
        auto* task = F4SE::GetTaskInterface();
        if (!task)
        {
            if (a_onComplete) a_onComplete();
            return;
        }

        if (DeferIfOMODCacheNotReady(a_refID, a_rule, a_onComplete)) return;

        LogModifyBegin(a_refID, a_rule);

        task->AddTask([a_refID, a_rule, cb = std::move(a_onComplete)]() mutable {
            RunModifyMainTask(a_refID, std::move(a_rule), std::move(cb));
        });
    }

} // namespace

// Fire-and-forget. Kept for direct callers.
void ApplyModifications(RE::Actor* a_actor, const SwapRuleResolved& a_rule)
{
    if (!a_actor) return;
    F4SE::GetTaskInterface()->AddTask([a_actor, a_rule]() { ApplyAll(a_actor, a_rule); });
}

// Repair pass. See the header for why only ApplySpecial belongs here.
void ReapplyIdempotent(RE::Actor* a_actor, const SwapRuleResolved& a_rule)
{
    if (!a_actor) return;
    ApplySpecial(a_actor, a_rule);
}

void OnAttachComplete(std::uint32_t a_refActor, std::uint32_t a_refItem, bool a_wasEquipped, bool a_attached)
{
    if (a_attached && a_wasEquipped) { RegisterPendingEquip(a_refActor, a_refItem); }
}

void OnEquipComplete(std::uint32_t a_refActor, std::uint32_t /*a_refItem*/, bool /*a_equipped*/)
{
    std::size_t remaining = 0;
    auto cb = DecrementEquipBatch(a_refActor, remaining);
    if (cb) cb();
}

// Pipeline entry. a_onComplete fires exactly once, after the three-phase
// chain finishes (cache gate, attach phase + settle delay, re-equip phase
// gated on per-item BNSOnEquipComplete callbacks). See the helpers in the
// anonymous namespace above for the body.
//
// DO NOT replace the per-item Papyrus EquipItem dispatch with a global
// Disable/Enable refresh — that re-inits worn items from the new base's
// default outfit and silently swaps every modded item for a fresh unmodded
// copy. Symptom: workbench shows no mods.
void ApplyModificationsAsync(std::uint32_t a_refID, const SwapRuleResolved& a_rule, std::function<void()> a_onComplete)
{ ApplyModificationsAsyncImpl(a_refID, a_rule, std::move(a_onComplete)); }

} // namespace BNS::Modifier
