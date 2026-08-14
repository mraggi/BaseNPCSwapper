#include "NPCEvaluator.hpp"
#include "Utils.hpp"
#include <RE/Fallout.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <random>
#include <spdlog/spdlog.h>

namespace BNS::Evaluator
{

namespace
{
    // Intersection check: does the actor's parent-location chain overlap
    // the rule's filter list?
    bool LocationMatches(const std::vector<RE::BGSLocation*>& a_activeLocs, const std::vector<RE::BGSLocation*>& a_filters)
    {
        for (auto* filterLoc : a_filters)
        {
            if (!filterLoc) continue;
            for (auto* activeLoc : a_activeLocs)
            {
                if (activeLoc && activeLoc == filterLoc) return true;
            }
        }
        return false;
    }
} // namespace

ActorInfo::ActorInfo(RE::Actor* actor)
{
    auto* baseObj = actor ? actor->data.objectReference : nullptr;
    auto* npc = baseObj ? baseObj->As<RE::TESNPC>() : nullptr;

    auto* fullNameComp = npc ? static_cast<RE::TESFullName*>(npc) : nullptr;
    name = (fullNameComp && fullNameComp->GetFullName()) ? fullNameComp->GetFullName() : "Unknown";

    refID = actor ? actor->GetFormID() : 0;
    RE::TESForm* trueBase = Utils::GetTrueBaseForm(actor);
    baseID = trueBase ? trueBase->GetFormID() : 0;
    level = actor ? actor->GetLevel() : 0;

    isUnique = npc ? npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kUnique) : false;
    isEssential = npc ? npc->IsEssential() : false;

    race = npc ? npc->formRace : nullptr;
}

std::string ActorInfo::ToString() const
{
    uint32_t displayRef = refID & 0xFFFFFF;
    uint32_t displayBase = ((baseID >> 24) == 0xFE) ? (baseID & 0xFFF) : (baseID & 0xFFFFFF);
    return std::format("['{}'|Ref:{:X}|Base:{:X}|Lvl:{}]", name, displayRef, displayBase, level);
}

double CalculateChance(int a_level, const SwapRuleResolved& a_rule)
{
    if ((a_level < a_rule.minLevel) || (a_level > a_rule.maxLevel)) return 0.;

    if (a_rule.ChanceScalingPerLevel == 0.0) return a_rule.chanceMin;

    double range = static_cast<double>(a_rule.maxLevel - a_rule.minLevel);
    if (range <= 0.0) return a_rule.chanceMin;

    double scaleFactor = static_cast<double>(a_level - a_rule.minLevel);

    double scaledChance = a_rule.chanceMin + (a_rule.ChanceScalingPerLevel * scaleFactor);

    return std::clamp(scaledChance, a_rule.chanceMin, a_rule.chanceMax);
}

bool RollChanceSeeded(double a_percentage, std::uint64_t a_seed)
{
    if (a_percentage >= 100.0) return true;
    if (a_percentage <= 0.0) return false;

    std::mt19937_64 rng(a_seed);
    std::uniform_real_distribution<double> dist(0.0, 100.0);
    return dist(rng) <= a_percentage;
}

MatchResult EvaluateActorMatch(RE::Actor* a_actor, const ActorInfo& a_info, const SwapRuleResolved& a_rule)
{
    if (a_rule.skipUniques && a_info.isUnique) { return {false, 0.0, MatchFailReason::Unique, "Actor is Unique."}; }
    if (a_rule.skipEssentials && a_info.isEssential)
    {
        return {false, 0.0, MatchFailReason::Essential, "Actor is Essential."};
    }

    if (a_rule.requiredBase && a_info.baseID != a_rule.requiredBase->GetFormID())
    {
        return {false,
                0.0,
                MatchFailReason::BaseID,
                std::format("BaseID {:X} does not match required {:X}", a_info.baseID, a_rule.requiredBase->GetFormID())};
    }

    for (auto* exclBase : a_rule.excludedBases)
    {
        if (exclBase && a_info.baseID == exclBase->GetFormID())
        {
            return {false, 0.0, MatchFailReason::ExcludedBaseID, std::format("BaseID {:X} is excluded", a_info.baseID)};
        }
    }

    if (a_rule.requiredFaction && !a_actor->IsInFaction(a_rule.requiredFaction))
    {
        return {false,
                0.0,
                MatchFailReason::Faction,
                std::format("Missing Faction {:X}", a_rule.requiredFaction->GetFormID())};
    }

    if (a_rule.requiredRace && a_info.race && a_info.race->GetFormID() != a_rule.requiredRace->GetFormID())
    {
        return {false,
                0.0,
                MatchFailReason::Race,
                std::format("Race {:X} does not match required {:X}",
                            a_info.race->GetFormID(),
                            a_rule.requiredRace->GetFormID())};
    }

    bool locFail = false;
    std::string locContext = "";
    auto* currentCell = a_actor->GetParentCell();
    RE::TESWorldSpace* currentWorldSpace = currentCell ? currentCell->worldSpace : nullptr;
    auto* currentLocation = a_actor->GetCurrentLocation();

    const bool needsLocChain = currentLocation
      && (!a_rule.requiredLocations.empty() || !a_rule.excludedLocations.empty());
    std::vector<RE::BGSLocation*> actorLocs;
    if (needsLocChain)
    {
        for (auto* loc = currentLocation; loc; loc = loc->parentLoc)
            actorLocs.push_back(loc);
    }

    if (!a_rule.requiredCells.empty() || !a_rule.requiredWorldSpaces.empty() || !a_rule.requiredLocations.empty())
    {
        bool matchFound = false;
        if (currentCell
            && std::find(a_rule.requiredCells.begin(), a_rule.requiredCells.end(), currentCell)
              != a_rule.requiredCells.end())
            matchFound = true;
        if (!matchFound && currentWorldSpace
            && std::find(a_rule.requiredWorldSpaces.begin(), a_rule.requiredWorldSpaces.end(), currentWorldSpace)
              != a_rule.requiredWorldSpaces.end())
            matchFound = true;
        if (!matchFound && !actorLocs.empty())
        {
            if (LocationMatches(actorLocs, a_rule.requiredLocations)) matchFound = true;
        }
        if (!matchFound)
        {
            locFail = true;
            locContext = "Outside required Location/Cell/Worldspace.";
        }
    }

    if (!locFail
        && (!a_rule.excludedCells.empty() || !a_rule.excludedWorldSpaces.empty() || !a_rule.excludedLocations.empty()))
    {
        if (currentCell
            && std::find(a_rule.excludedCells.begin(), a_rule.excludedCells.end(), currentCell)
              != a_rule.excludedCells.end())
        {
            locFail = true;
            locContext = "Inside excluded Cell.";
        }
        else if (currentWorldSpace
                 && std::find(a_rule.excludedWorldSpaces.begin(), a_rule.excludedWorldSpaces.end(), currentWorldSpace)
                   != a_rule.excludedWorldSpaces.end())
        {
            locFail = true;
            locContext = "Inside excluded Worldspace.";
        }
        else if (!actorLocs.empty() && LocationMatches(actorLocs, a_rule.excludedLocations))
        {
            locFail = true;
            locContext = "Inside excluded Location.";
        }
    }

    // Interior/exterior is the CELL flag (what the Creation Kit shows), so an
    // actor with no parent cell simply cannot be classified — fail closed
    // rather than guess.
    if (!locFail && a_rule.interiorState != InteriorCondition::kIgnore)
    {
        if (!currentCell)
        {
            locFail = true;
            locContext = "Interior/exterior filter set but actor has no parent cell.";
        }
        else if (a_rule.interiorState == InteriorCondition::kMustBeInterior && !currentCell->IsInterior())
        {
            locFail = true;
            locContext = "Must be in an interior cell (but is outside).";
        }
        else if (a_rule.interiorState == InteriorCondition::kMustBeExterior && currentCell->IsInterior())
        {
            locFail = true;
            locContext = "Must be in an exterior cell (but is inside).";
        }
    }

    if (locFail) { return {false, 0.0, MatchFailReason::Location, locContext}; }

    for (auto* kw : a_rule.excludedKeywords)
    {
        if (kw && a_actor->HasKeyword(kw))
        {
            return {false,
                    0.0,
                    MatchFailReason::ExcludedKeyword,
                    std::format("Has excluded Keyword {:X}", kw->GetFormID())};
        }
    }

    for (auto* kw : a_rule.requiredKeywords)
    {
        if (kw && !a_actor->HasKeyword(kw))
        {
            return {false,
                    0.0,
                    MatchFailReason::RequiredKeyword,
                    std::format("Missing required Keyword {:X}", kw->GetFormID())};
        }
    }

    if (a_rule.powerArmorState != PowerArmorCondition::kIgnore)
    {
        bool wearingPA = a_actor->extraList && a_actor->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor);
        if (a_rule.powerArmorState == PowerArmorCondition::kMustWear && !wearingPA)
        {
            return {false, 0.0, MatchFailReason::PowerArmor, "Must be wearing Power Armor (but isn't)."};
        }
        if (a_rule.powerArmorState == PowerArmorCondition::kMustNotWear && wearingPA)
        {
            return {false, 0.0, MatchFailReason::PowerArmor, "Must NOT be wearing Power Armor (but is)."};
        }
    }

    std::string lowerName = a_info.name;
    std::ranges::transform(lowerName, lowerName.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const auto& substr : a_rule.nameMustContain)
    {
        if (lowerName.find(substr) == std::string::npos)
        {
            return {false, 0.0, MatchFailReason::NameMatch, std::format("Name missing '{}'", substr)};
        }
    }

    for (const auto& substr : a_rule.nameMustNotContain)
    {
        if (lowerName.find(substr) != std::string::npos)
        {
            return {false, 0.0, MatchFailReason::NameMatch, std::format("Name contains excluded '{}'", substr)};
        }
    }

    double chance = CalculateChance(a_info.level, a_rule);

    if (a_info.level < a_rule.minLevel || a_info.level > a_rule.maxLevel)
    {
        std::string maxLvlStr = a_rule.maxLevel == std::numeric_limits<int>::max() ? "MAX"
                                                                                   : std::to_string(a_rule.maxLevel);
        return {false,
                0.0,
                MatchFailReason::Level,
                std::format("Level {} outside range [{} ~ {}]", a_info.level, a_rule.minLevel, maxLvlStr)};
    }
    if (chance == 0.0) { return {false, 0.0, MatchFailReason::Chance, "Calculated trigger chance is 0%"}; }

    return {true, chance, MatchFailReason::None, ""};
}

void LogDebugResult(const MatchResult& res, const ActorInfo& info, const SwapRuleResolved& rule)
{
    if (rule.debugLevel <= 0) return;

    // Level 1 ONLY logs when chance > 0
    if ((rule.debugLevel >= 1) && (res.chance > 0.))
    {
        spdlog::info("[{} | {}] RULE MATCHED actor {}  with chance {}",
                     rule.sourceFile,
                     rule.ruleName,
                     info.ToString(),
                     res.chance);
        return;
    }

    if (res.success) return;

    if (rule.debugLevel >= 2)
    {
        // Level 2 silences the noisy early-exit reasons; 3+ logs them too.
        bool isEarlyFail = (res.failReason == MatchFailReason::Unique || res.failReason == MatchFailReason::Essential
                            || res.failReason == MatchFailReason::BaseID
                            || res.failReason == MatchFailReason::ExcludedBaseID
                            || res.failReason == MatchFailReason::Faction || res.failReason == MatchFailReason::Race
                            || res.failReason == MatchFailReason::Location);

        if (!isEarlyFail)
        {
            spdlog::debug("[{} | {}] EVALUATION FAILED | {} | Reason: {}",
                          rule.sourceFile,
                          rule.ruleName,
                          info.ToString(),
                          res.failContext);
            return;
        }

        if (rule.debugLevel >= 3)
        {
            spdlog::debug("[{} | {}] EVALUATION FAILED | {} | Reason: {}",
                          rule.sourceFile,
                          rule.ruleName,
                          info.ToString(),
                          res.failContext);
        }
    }
}
} // namespace BNS::Evaluator
