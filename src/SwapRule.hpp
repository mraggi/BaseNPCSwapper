#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

enum class PowerArmorCondition
{
    kIgnore = 0,
    kMustWear = 1,
    kMustNotWear = 2
};

struct SwapRule
{
    // Meta info
    std::string sourceFile = "Unknown.ini";
    size_t sourceLine = 0;
    std::string ruleName = "";
    int debugLevel = 0;
    double sortOrder = 0;

    // --- Filters ---
    std::string requiredBaseStr = "";
    std::vector<std::string> excludedBaseStrs;

    std::string requiredFactionStr = "";
    std::string requiredRaceStr = "";

    std::vector<std::string> requiredKeywordStrs;
    std::vector<std::string> excludedKeywordStrs;

    std::vector<std::string> nameMustContain;
    std::vector<std::string> nameMustNotContain;
    PowerArmorCondition powerArmorState = PowerArmorCondition::kIgnore;

    std::vector<std::string> locationalFilters;
    std::vector<std::string> locationalFiltersExcluded;

    int minLevel = 0;
    int maxLevel = std::numeric_limits<int>::max();

    double chanceMin = 100.0;
    double chanceMax = 100.0;
    double ChanceScalingPerLevel = 0.0;

    bool skipUniques = true;
    bool skipEssentials = true;

    // --- Dev/testing ---
    // When true, the rule matches and logs normally but skips every mutation
    // (spawn, replaceBy, factions, items, OMODs). Also does NOT mark the rule
    // as applied — clearing dryRun in a later session will let the rule
    // actually fire on the same actor.
    bool dryRun = false;

    // --- Middle actions ---
    std::string replaceByStr = "";
    std::string spawnStr = "";

    // --- Post Modifications ---
    std::vector<std::string> addFactionsStrs;
    std::vector<std::string> addItemsStrs;
    std::vector<std::string> addOMODsStrs;
    bool OMODRandomizationPerItem = true;

    // --- SPECIAL boost (boostSpecialByLevel) ---
    // Raise base SPECIAL toward a level-scaled target total, distributing the
    // shortfall randomly across the 7 stats (floor-to-target: never lowers a
    // stat). target = clamp(specialBaseline + specialPerLevel*level, baseline, maxTotal).
    bool boostSpecial = false;
    float specialBaseline = 10.0f; // target total for a level-0 NPC
    float specialPerLevel = 0.25f; // extra target points per character level
    float specialMaxTotal = 50.0f; // cap on the target total
    int specialPerStatCap = 10; // per-stat ceiling (engine SPECIAL max is 10)
};
