#include "NPCManager.hpp"
#include "DelayManager.hpp"
#include "NPCEvaluator.hpp"
#include "NPCModifier.hpp"
#include "NPCSwapper.hpp"
#include "Utils.hpp"

#include <F4SE/F4SE.h>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace BNS
{

namespace
{

    constexpr int kStabilizePollMS = 32;
    constexpr int kStabilizeMaxTries = 256;
    constexpr int kPreModifySettleMS = 96;

    // Eviction cap. Soft — counted in actor entries, not (actor, rule) pairs.
    constexpr std::size_t kProcessedActorSoftCap = 10000;

    RE::Actor* FetchActor(std::uint32_t a_refID)
    {
        auto* form = RE::TESForm::GetFormByID(a_refID);
        // GetFormByID happily returns forms that have been MarkForDelete'd
        // but not yet GC'd. SetObjectReference on those leaks extras.
        if (!form || form->IsDeleted()) return nullptr;
        return form->As<RE::Actor>();
    }

    bool HasAnyModifications(const SwapRuleResolved& a_rule)
    {
        return !a_rule.addOMODs.empty() || !a_rule.addFactions.empty() || !a_rule.addItems.empty() || a_rule.boostSpecial;
    }

    // Bodyguard spawn: dispatches a Papyrus helper which places the spawn
    // form at the original actor's location. Fire-and-forget — the spawned
    // actor re-enters the pipeline via its own Load3D hook.
    void PerformSpawnAlongside(std::uint32_t a_originalRefID, const SwapRuleResolved& a_rule)
    {
        if (a_rule.debugLevel >= 1)
        {
            auto* refForm = RE::TESForm::GetFormByID(a_originalRefID);
            spdlog::debug("*************** Spawning BODYGUARD {} right next to {}",
                          Utils::GetFormName(a_rule.spawn),
                          Utils::GetFormName(refForm));
        }

        Utils::DispatchToPapyrus("BNSSpawnHelper", "Spawn", a_originalRefID, a_rule.spawn->GetFormID());
    }

} // namespace

NPCManager* NPCManager::GetSingleton()
{
    static NPCManager singleton;
    return &singleton;
}

void NPCManager::SetRules(std::vector<SwapRuleResolved>&& a_rules)
{
    m_rules = std::move(a_rules);

    // Ordered rule-set fingerprint: FNV-1a over the per-rule hashes in
    // resolved-list order. Different fingerprint ⇒ rules added / removed /
    // edited / reordered ⇒ session set is not safe to restore.
    auto fp = Utils::kFNV1aBasis;
    for (const auto& r : m_rules)
        fp = Utils::FNV1aChain(fp, r.hash);
    m_ruleSetFingerprint = fp;

    m_repairableRules.clear();
    for (std::size_t i = 0; i < m_rules.size(); ++i)
    {
        if (m_rules[i].boostSpecial && !m_rules[i].dryRun) m_repairableRules.push_back(i);
    }
}

void NPCManager::StartPipeline()
{
    m_isShuttingDown = false;
    DelayManager::Start();
    spdlog::info("BNS pipeline started: {} rule(s) active, ready to evaluate actors.", m_rules.size());
    spdlog::debug("(Internal rule-set fingerprint: {:016X}.)", m_ruleSetFingerprint);
}

void NPCManager::RequestShutdown() { m_isShuttingDown = true; }

bool NPCManager::IsShuttingDown() const { return m_isShuttingDown.load(); }

void NPCManager::BeginReload() { m_isReloading = true; }

void NPCManager::EndReload() { m_isReloading = false; }

bool NPCManager::IsReloading() const { return m_isReloading.load(); }

void NPCManager::ClearSessionActorsOnly()
{
    std::size_t cleared = 0;
    {
        std::lock_guard lock(m_actorMutex);
        cleared = m_sessionActors.size();
        m_sessionActors.clear();
    }
    spdlog::info("[Reload] Cleared {} session-actor entries — every live actor will re-evaluate on next Load3D.",
                 cleared);
}

bool NPCManager::IsPipelineEmpty() const
{
    std::lock_guard lock(m_actorMutex);
    return m_inProgressActors.empty();
}

bool NPCManager::TryMarkInProgress(std::uint32_t a_refID)
{
    std::lock_guard lock(m_actorMutex);
    if (m_sessionActors.contains(a_refID)) return false; // fast-skip: already walked this session
    if (m_inProgressActors.contains(a_refID)) return false; // pipeline already running for this actor
    m_inProgressActors.insert(a_refID);
    return true;
}

void NPCManager::FinalizeActor(std::uint32_t a_refID)
{
    std::lock_guard lock(m_actorMutex);
    m_inProgressActors.erase(a_refID);
    m_sessionActors.insert(a_refID); // hard-skip for the rest of the session
}

bool NPCManager::IsRuleAlreadyApplied(std::uint32_t a_refID, std::uint64_t a_ruleHash) const
{
    std::lock_guard lock(m_actorMutex);
    auto it = m_processedActors.find(a_refID);
    if (it == m_processedActors.end()) return false;
    return it->second.matchedRules.contains(a_ruleHash);
}

void NPCManager::MarkRuleApplied(std::uint32_t a_refID, std::uint64_t a_ruleHash, bool a_persistent)
{
    std::lock_guard lock(m_actorMutex);
    auto& entry = m_processedActors[a_refID];
    entry.matchedRules.insert(a_ruleHash);
    entry.lastTouchedTick = ++m_nextTick;
    entry.persistent = entry.persistent || a_persistent;
}

void NPCManager::EvictOldestIfNeeded()
{
    std::lock_guard lock(m_actorMutex);
    if (m_processedActors.size() <= kProcessedActorSoftCap) return;

    // Collect non-persistent entries with their ticks.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> candidates;
    candidates.reserve(m_processedActors.size());
    for (const auto& [refID, e] : m_processedActors)
    {
        if (!e.persistent) candidates.emplace_back(e.lastTouchedTick, refID);
    }
    std::ranges::sort(candidates); // ascending by tick → oldest first

    const std::size_t toDrop = m_processedActors.size() - kProcessedActorSoftCap;
    const std::size_t cap = std::min(toDrop, candidates.size());
    for (std::size_t i = 0; i < cap; ++i)
        m_processedActors.erase(candidates[i].second);

    if (m_processedActors.size() > kProcessedActorSoftCap)
    {
        spdlog::warn("[Pipeline] Eviction left {} actors in cache ({} persistent), above soft cap {}.",
                     m_processedActors.size(),
                     m_processedActors.size() - candidates.size() + cap,
                     kProcessedActorSoftCap);
    }
    else
    {
        spdlog::info("[Pipeline] Evicted {} actor(s); {} remain.", cap, m_processedActors.size());
    }
}

void NPCManager::ClearProcessedActors()
{
    std::size_t cleared = 0;
    {
        std::lock_guard lock(m_actorMutex);
        cleared = m_sessionActors.size() + m_processedActors.size();
        m_sessionActors.clear();
        m_processedActors.clear();
        m_nextTick = 1;
    }
    spdlog::info("Cleared {} cache entries. All actors will be re-evaluated on next load.", cleared);
}

NPCManager::Snapshot NPCManager::GetSnapshot() const
{
    std::lock_guard lock(m_actorMutex);
    Snapshot s;
    s.sessionActors = m_sessionActors;
    s.processedActors = m_processedActors;
    s.nextTick = m_nextTick;
    return s;
}

void NPCManager::SetSnapshot(Snapshot&& a_snap)
{
    std::lock_guard lock(m_actorMutex);
    m_sessionActors = std::move(a_snap.sessionActors);
    m_processedActors = std::move(a_snap.processedActors);
    m_nextTick = a_snap.nextTick;

    // GC stale rule hashes: a persistent unique actor accumulates hashes
    // across sessions, but rules get edited / removed. Filter against the
    // currently loaded rule set so the co-save doesn't keep growing dead
    // hashes per actor. The V1/V2 migration sentinel is preserved so those
    // entries still skip the whole rule list.
    std::unordered_set<std::uint64_t> liveHashes;
    liveHashes.reserve(m_rules.size() + 1);
    for (const auto& r : m_rules)
        liveHashes.insert(r.hash);

    std::size_t prunedHashes = 0;
    std::size_t prunedEntries = 0;
    for (auto it = m_processedActors.begin(); it != m_processedActors.end();)
    {
        auto& set = it->second.matchedRules;
        for (auto h = set.begin(); h != set.end();)
        {
            // V1/V2 migration sentinel (0xFFFF…FFFF) means "fully processed
            // against the unknown prior rule set". Keep it.
            constexpr std::uint64_t kMigratedSentinel = 0xFFFFFFFFFFFFFFFFULL;
            if (*h == kMigratedSentinel || liveHashes.contains(*h)) { ++h; }
            else
            {
                h = set.erase(h);
                ++prunedHashes;
            }
        }
        if (set.empty() && !it->second.persistent)
        {
            it = m_processedActors.erase(it);
            ++prunedEntries;
        }
        else
        {
            ++it;
        }
    }

    if (prunedHashes > 0 || prunedEntries > 0)
    {
        spdlog::info("[Load] Pruned {} stale rule hash(es) and {} now-empty actor entry(ies) "
                     "from per-actor history.",
                     prunedHashes,
                     prunedEntries);
    }
}

// Stage 0 — Load3D hook entry. Wait for 3D + FaceGen before evaluating.
void NPCManager::QueueDeferredEvaluation(std::uint32_t a_formID)
{
    if (m_isShuttingDown || m_isReloading) return;
    if (!TryMarkInProgress(a_formID))
    {
        // Already walked (this session or a restored one) — the full pipeline
        // stays skipped, but idempotent effects still need re-asserting.
        QueueRepairPass(a_formID);
        return;
    }

    auto ready = [](RE::Actor* a_actor, int) -> bool {
        if (!a_actor->Get3D()) return false;
        auto* process = a_actor->currentProcess;
        auto* high = process ? process->high : nullptr;
        if (high && high->faceGenLoadPending) return false;
        return true;
    };

    auto onReady = [this](RE::Actor* a_actor, int) {
        // Skip corpses streamed in dead. The rule walk re-checks every
        // iteration anyway; this just avoids spinning up the chain.
        if (a_actor->IsDead(false))
        {
            FinishActorPipeline(a_actor->GetFormID());
            return;
        }
        EvaluateRuleAt(a_actor->GetFormID(), 0);
    };

    auto onTimeout = [this](RE::Actor* a_actor) {
        spdlog::warn("[Pipeline] Stabilization timeout for actor [{:08X}] - aborting.", a_actor->GetFormID());
        FinishActorPipeline(a_actor->GetFormID());
    };

    auto onFail = [this](std::uint32_t a_failedID) { FinishActorPipeline(a_failedID); };

    DelayManager::QueuePoller(a_formID, kStabilizePollMS, kStabilizeMaxTries, ready, onReady, onTimeout, onFail);
}

// Stage 0b — the actor already finished the walk, so the pipeline is skipped.
// Engine state that BNS wrote may have been reverted since (cell reset,
// auto-calc re-derivation, a template re-resolve on a later Disable→Enable),
// and a level-scaled target drifts as the actor levels with the player. Both
// need the rule's idempotent effects re-asserted on every load.
//
// Deliberately NOT a re-walk: only rules already in this actor's matchedRules
// are touched, and only through Modifier::ReapplyIdempotent. No filter is
// re-evaluated, so a rule that would now match the actor's post-swap base form
// still cannot fire and cascade.
void NPCManager::QueueRepairPass(std::uint32_t a_refID)
{
    if (m_repairableRules.empty()) return;

    std::vector<std::size_t> toRepair;
    {
        std::lock_guard lock(m_actorMutex);

        // A live pipeline applies these itself when it reaches the rule.
        if (m_inProgressActors.contains(a_refID)) return;

        auto it = m_processedActors.find(a_refID);
        if (it == m_processedActors.end()) return;

        const auto& applied = it->second.matchedRules;
        for (auto idx : m_repairableRules)
        {
            if (applied.contains(m_rules[idx].hash)) toRepair.push_back(idx);
        }
    }
    if (toRepair.empty()) return;

    auto* task = F4SE::GetTaskInterface();
    if (!task) return;

    task->AddTask([this, a_refID, toRepair = std::move(toRepair)]() {
        auto* actor = FetchActor(a_refID);
        if (!actor || actor->IsDead(false)) return;
        for (auto idx : toRepair)
            Modifier::ReapplyIdempotent(actor, m_rules[idx]);
    });
}

// Re-fetch by FormID every iteration so prior rule mutations (swap, faction,
// items) are visible and despawned actors are detected. Non-matching rules
// stay in the in-function loop; async hops only happen on a real match.
void NPCManager::EvaluateRuleAt(std::uint32_t a_refID, std::size_t a_startIdx)
{
    for (std::size_t idx = a_startIdx; idx < m_rules.size(); ++idx)
    {
        if (m_isShuttingDown)
        {
            spdlog::debug("[Pipeline] Shutdown - actor [{:08X}] aborting at rule {}.", a_refID, idx);
            FinishActorPipeline(a_refID);
            return;
        }

        auto* actor = FetchActor(a_refID);
        if (!actor)
        {
            spdlog::debug("[Pipeline] Actor [{:08X}] vanished mid-pipeline at rule {}.", a_refID, idx);
            FinishActorPipeline(a_refID);
            return;
        }

        // IsDead(false) catches essentials at 0 HP too. Re-checked every
        // rule — an actor can die mid-pipeline (combat, damaging cell).
        if (actor->IsDead(false))
        {
            spdlog::debug("[Pipeline] Actor [{:08X}] is dead at rule {} — aborting.", a_refID, idx);
            FinishActorPipeline(a_refID);
            return;
        }

        const auto& rule = m_rules[idx];
        Evaluator::ActorInfo info(actor);
        auto res = Evaluator::EvaluateActorMatch(actor, info, rule);
        Evaluator::LogDebugResult(res, info, rule);

        const std::uint64_t seed = static_cast<std::uint64_t>(a_refID) ^ rule.hash;
        const bool matched = res.success && (res.chance > 0.0) && Evaluator::RollChanceSeeded(res.chance, seed);

        if (!matched) continue;

        // Skip if this rule already fired on this actor in a prior session.
        if (IsRuleAlreadyApplied(a_refID, rule.hash))
        {
            if (rule.debugLevel >= 2)
            {
                spdlog::debug("[Pipeline] Actor [{:08X}] rule '{}' already applied — skipping.", a_refID, rule.ruleName);
            }
            continue;
        }

        ExecuteMatchingRule(a_refID, idx);
        return; // ExecuteMatchingRule re-enters EvaluateRuleAt(idx+1) when its
                // async chain completes.
    }

    // Walked the whole list without an early-exit match - we're done.
    FinishActorPipeline(a_refID);
}

// Matched rule actions, in order:
//   1. spawnAlongside — fire-and-forget; spawned actor enters via its own Load3D.
//   2. replaceBy      — async; refID preserved, base form changes.
//   3. modify         — async; factions/items immediate, OMODs via Papyrus.
void NPCManager::ExecuteMatchingRule(std::uint32_t a_refID, std::size_t a_ruleIdx)
{
    const auto& rule = m_rules[a_ruleIdx];

    // Dry-run: log the match, skip ALL mutations (spawn / swap / modify),
    // do NOT mark as applied so removing dryRun later re-evaluates cleanly.
    if (rule.dryRun)
    {
        if (rule.debugLevel >= 1)
        {
            auto* refForm = RE::TESForm::GetFormByID(a_refID);
            spdlog::info("[{} | {}] [DryRun] WOULD APPLY to '{}' [{:08X}] — "
                         "spawn={}, replaceBy={}, +{} faction(s), +{} item(s), +{} OMOD(s), boostSpecial={}.",
                         rule.sourceFile,
                         rule.ruleName,
                         refForm ? Utils::GetFormName(refForm) : "<unknown>",
                         a_refID,
                         rule.spawn ? Utils::GetFormName(rule.spawn) : "<none>",
                         rule.replaceBy ? Utils::GetFormName(rule.replaceBy) : "<none>",
                         rule.addFactions.size(),
                         rule.addItems.size(),
                         rule.addOMODs.size(),
                         rule.boostSpecial);
        }
        if (auto* t = F4SE::GetTaskInterface())
        {
            t->AddTask([this, a_refID, a_ruleIdx]() { EvaluateRuleAt(a_refID, a_ruleIdx + 1); });
        }
        else
        {
            EvaluateRuleAt(a_refID, a_ruleIdx + 1);
        }
        return;
    }

    // Capture persistence at decision time — the actor pointer may not be
    // valid by the time MarkRuleApplied fires (chain runs async).
    bool persistent = false;
    if (auto* actorAtMatch = FetchActor(a_refID))
    {
        Evaluator::ActorInfo info(actorAtMatch);
        persistent = info.isUnique || info.isEssential;
    }
    const std::uint64_t ruleHash = rule.hash;

    // advanceToNextRule MUST run on the main thread: the evaluator reads
    // engine state (extraList, factions, keywords, parent cell) which races
    // mid-pipeline mutation otherwise. Callers from the DelayManager worker
    // (drain QueueTask in ApplyModificationsAsync; the settle QueueTask
    // below) would crash without the hop. This was the "rare crashes that
    // vanish under debug logging" bug — logging widened the race window
    // enough to close it.
    auto advanceToNextRule = [this, a_refID, a_ruleIdx, ruleHash, persistent]() {
        MarkRuleApplied(a_refID, ruleHash, persistent);
        if (auto* t = F4SE::GetTaskInterface())
        {
            t->AddTask([this, a_refID, a_ruleIdx]() { EvaluateRuleAt(a_refID, a_ruleIdx + 1); });
        }
        else
        {
            EvaluateRuleAt(a_refID, a_ruleIdx + 1);
        }
    };

    auto applyModifications = [this, a_refID, a_ruleIdx, advanceToNextRule]() {
        const auto& r = m_rules[a_ruleIdx];
        if (!HasAnyModifications(r))
        {
            advanceToNextRule();
            return;
        }
        Modifier::ApplyModificationsAsync(a_refID, r, advanceToNextRule);
    };

    // Always run modifications through a small settle delay — even with
    // the Phase-8 Papyrus callback in place, give the engine ~3 frames
    // for equip state to fully reconcile before SafeAttachMods touches
    // anything. Belt-and-braces against any remaining post-swap settle.
    auto applyReplace = [this, a_refID, a_ruleIdx, applyModifications]() {
        const auto& rule = m_rules[a_ruleIdx];
        auto settleThenApply = [applyModifications]() {
            DelayManager::QueueTask(kPreModifySettleMS, applyModifications);
        };
        if (!rule.replaceBy)
        {
            settleThenApply();
            return;
        }
        Swapper::PerformSwap(a_refID, rule, settleThenApply);
    };

    // Step 1: spawn (fire-and-forget; doesn't gate the chain).
    if (rule.spawn && rule.spawn->As<RE::TESBoundObject>()) { PerformSpawnAlongside(a_refID, rule); }

    // Step 2 -> Step 3 -> advance.
    applyReplace();
}

// Single finalization point — every chain end (success/abort/shutdown/vanished) lands here.
void NPCManager::FinishActorPipeline(std::uint32_t a_refID) { FinalizeActor(a_refID); }

} // namespace BNS
