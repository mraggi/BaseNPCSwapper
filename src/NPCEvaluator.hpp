#pragma once

#include "SwapRuleResolved.hpp"
#include <RE/Fallout.h>
#include <string>
#include <vector>

namespace BNS::Evaluator
{

struct ActorInfo
{
    std::string name;
    uint32_t refID;
    uint32_t baseID;
    int level;
    bool isUnique;
    bool isEssential;
    RE::TESRace* race; // Native Race caching

    explicit ActorInfo(RE::Actor* actor);
    [[nodiscard]] std::string ToString() const;
};

enum class MatchFailReason
{
    None,
    Unique,
    Essential,
    BaseID,
    ExcludedBaseID,
    Faction,
    Race,
    Location,
    ExcludedKeyword,
    RequiredKeyword,
    PowerArmor,
    NameMatch,
    Level,
    Chance
};

struct MatchResult
{
    bool success = false;
    double chance = 0.0;
    MatchFailReason failReason = MatchFailReason::None;
    std::string failContext = "";
};

double CalculateChance(int a_level, const SwapRuleResolved& a_rule);

// Deterministic per (actor, rule). Same seed → same roll across saves /
// restarts / reloads. Caller passes seed = refID ^ rule.hash.
bool RollChanceSeeded(double a_percentage, std::uint64_t a_seed);

MatchResult EvaluateActorMatch(RE::Actor* a_actor, const ActorInfo& a_info, const SwapRuleResolved& a_rule);
void LogDebugResult(const MatchResult& res, const ActorInfo& info, const SwapRuleResolved& rule);

} // namespace BNS::Evaluator
