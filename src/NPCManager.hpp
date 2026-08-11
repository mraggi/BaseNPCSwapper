#pragma once

#include "SwapRuleResolved.hpp"
#include <RE/Fallout.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BNS
{

// Per-(actor, rule) dedupe: every rule whose ExecuteMatchingRule fired for
// this actor lands here. lastTouchedTick is the LRU key; persistent==true
// (Unique or Essential actor) opts out of eviction.
struct ProcessedEntry
{
    std::unordered_set<std::uint64_t> matchedRules;
    std::uint64_t lastTouchedTick = 0;
    bool persistent = false;
};

// Owns the per-actor rule pipeline AND the actor-state caches. Each actor
// walks the rule list in a continuation-passing chain that hops between
// F4SE TaskInterface and DelayManager — no new threads. refID is stable
// across replaceBy (only the base form changes), so every step re-fetches
// by FormID.
//
//   [Load3D hook]
//        v
//   Stabilize (Get3D + faceGenLoadPending)
//        v
//   EvaluateRuleAt -> match? ExecuteMatchingRule (spawn → replace → modify → next idx)
//                 -> miss?  next idx
//                 -> end?   FinishActorPipeline
class NPCManager
{
public:
    static NPCManager* GetSingleton();

    // Computes the per-rule hash (already done in ResolveRules) and the
    // ordered rule-set fingerprint that gates session-set restore on load.
    void SetRules(std::vector<SwapRuleResolved>&& a_rules);
    void StartPipeline();

    // Only safe to mutate before MNAMResolver::IsCacheReady() flips. After
    // that the pipeline reads m_rules from the main thread concurrently.
    std::vector<SwapRuleResolved>& GetMutableRules() { return m_rules; }

    [[nodiscard]] std::uint64_t GetRuleSetFingerprint() const noexcept { return m_ruleSetFingerprint; }

    void QueueDeferredEvaluation(std::uint32_t a_formID);

    void RequestShutdown();
    [[nodiscard]] bool IsShuttingDown() const;
    [[nodiscard]] bool IsPipelineEmpty() const;

    // Reload gate: while set, the Load3D hook short-circuits so no new
    // actors enter the pipeline. Live in-flight pipelines drain naturally.
    // BeginReload / EndReload are called by the BNSReloadRules native
    // worker in main.cpp.
    void BeginReload();
    void EndReload();
    [[nodiscard]] bool IsReloading() const;

    // After a successful reload, every live actor needs to re-walk the
    // (possibly changed) rule list on its next Load3D. Wipes ONLY the
    // session set; the per-actor matchedRules history stays (rule-hash
    // dedup still works for unchanged rules; new/edited rules have new
    // hashes and will fire).
    void ClearSessionActorsOnly();

    // Pipeline state ops (replace the prior Swapper:: free functions).
    bool TryMarkInProgress(std::uint32_t a_refID);
    void FinalizeActor(std::uint32_t a_refID);

    // Per-rule dedupe.
    [[nodiscard]] bool IsRuleAlreadyApplied(std::uint32_t a_refID, std::uint64_t a_ruleHash) const;
    void MarkRuleApplied(std::uint32_t a_refID, std::uint64_t a_ruleHash, bool a_persistent);

    // Eviction. LRU on m_processedActors by lastTouchedTick, skipping
    // entries with persistent=true. Triggered post-Load and post-Clear.
    void EvictOldestIfNeeded();

    // MCM "Clear cache": wipes both session and persisted caches.
    void ClearProcessedActors();

    // Serialization snapshot accessors. Mutex held while copying.
    struct Snapshot
    {
        std::unordered_set<std::uint32_t> sessionActors;
        std::unordered_map<std::uint32_t, ProcessedEntry> processedActors;
        std::uint64_t nextTick = 1;
    };
    [[nodiscard]] Snapshot GetSnapshot() const;
    void SetSnapshot(Snapshot&& a_snap);

private:
    NPCManager() = default;
    ~NPCManager() = default;
    NPCManager(const NPCManager&) = delete;
    NPCManager& operator=(const NPCManager&) = delete;

    void EvaluateRuleAt(std::uint32_t a_refID, std::size_t a_startIdx);
    void ExecuteMatchingRule(std::uint32_t a_refID, std::size_t a_ruleIdx);
    void FinishActorPipeline(std::uint32_t a_refID);

    // Load3D for an actor that already finished the walk. The full pipeline is
    // still skipped — this only re-asserts the idempotent effects of rules that
    // already matched this actor (Modifier::ReapplyIdempotent). No filters are
    // re-evaluated and no new rule can fire, so a swapped actor can never
    // cascade into a rule that targets its new base form.
    void QueueRepairPass(std::uint32_t a_refID);

    std::vector<SwapRuleResolved> m_rules;
    // Indices into m_rules of the rules QueueRepairPass has anything to do for.
    // Empty for every rule set that uses no boostSpecialByLevel — the repair
    // pass then costs one vector-empty check per Load3D.
    std::vector<std::size_t> m_repairableRules;
    std::uint64_t m_ruleSetFingerprint = 0;

    mutable std::mutex m_actorMutex;
    std::unordered_set<std::uint32_t> m_inProgressActors;
    std::unordered_set<std::uint32_t> m_sessionActors;
    std::unordered_map<std::uint32_t, ProcessedEntry> m_processedActors;
    std::uint64_t m_nextTick = 1;

    std::atomic<bool> m_isShuttingDown {false};
    std::atomic<bool> m_isReloading {false};
};

} // namespace BNS
