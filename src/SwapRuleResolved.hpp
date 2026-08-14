#pragma once
#include "NPCModifier.hpp"
#include "SwapRule.hpp"
#include <RE/Fallout.h>
#include <limits>
#include <string>
#include <vector>

struct SwapRuleResolved
{
    // --- Meta ---
    std::string sourceFile;
    std::string ruleName;
    int debugLevel = 0;
    double sortOrder = 0.0;

    // --- Filters ---
    RE::TESForm* requiredBase = nullptr;
    std::vector<RE::TESForm*> excludedBases;

    RE::TESFaction* requiredFaction = nullptr;
    RE::TESRace* requiredRace = nullptr;

    std::vector<RE::BGSKeyword*> requiredKeywords;
    std::vector<RE::BGSKeyword*> excludedKeywords;

    std::vector<std::string> nameMustContain;
    std::vector<std::string> nameMustNotContain;

    PowerArmorCondition powerArmorState = PowerArmorCondition::kIgnore;
    InteriorCondition interiorState = InteriorCondition::kIgnore;

    // --- Locational ---
    std::vector<RE::TESObjectCELL*> requiredCells;
    std::vector<RE::TESWorldSpace*> requiredWorldSpaces;
    std::vector<RE::BGSLocation*> requiredLocations;

    std::vector<RE::TESObjectCELL*> excludedCells;
    std::vector<RE::TESWorldSpace*> excludedWorldSpaces;
    std::vector<RE::BGSLocation*> excludedLocations;

    // --- Skip flags ---
    bool skipUniques = true;
    bool skipEssentials = true;

    // When true, matches log normally but no mutation happens, and the rule
    // is not marked as applied (so clearing the flag re-evaluates next time).
    bool dryRun = false;

    // --- Level + chance ---
    int minLevel = 0;
    int maxLevel = std::numeric_limits<int>::max();
    double chanceMin = 100.0;
    double chanceMax = 100.0;
    double ChanceScalingPerLevel = 0.0;

    // --- Actions ---
    RE::TESForm* replaceBy = nullptr;
    RE::TESForm* spawn = nullptr;

    // --- Post-Spawn Modifications ---
    std::vector<RE::TESFaction*> addFactions;
    std::vector<RE::TESBoundObject*> addItems;
    std::vector<RE::BGSMod::Attachment::Mod*> addOMODs;

    // Built by Modifier::BuildOMODCache after MNAMResolver runs.
    BNS::Modifier::OMODDictionary OMODCache;
    // Default matches SwapRule's. ResolveRule always copies the parsed value,
    // so this only affects default-constructed instances — kept in sync to
    // avoid confusion.
    bool OMODRandomizationPerItem = true;

    // --- SPECIAL boost (see SwapRule for the formula) ---
    bool boostSpecial = false;
    float specialBaseline = 10.0f;
    float specialPerLevel = 0.25f;
    float specialMaxTotal = 50.0f;
    int specialPerStatCap = 10;

    // FNV-1a 64 over every behavior-affecting field. Stable across runs:
    // serialized into the co-save so re-loading detects rule changes per-actor.
    // Zero on default-constructed rules; populated by ResolveRules.
    std::uint64_t hash = 0;
};

std::vector<SwapRuleResolved> ResolveRules(const std::vector<SwapRule>& rules);
