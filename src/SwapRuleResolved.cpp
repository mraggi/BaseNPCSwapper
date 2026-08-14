#include "SwapRuleResolved.hpp"
#include "Utils.hpp"

#include <RE/Fallout.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <limits>
#include <spdlog/spdlog.h>
#include <string>

namespace
{

std::string ToLower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Null-safe stand-in for TESDataHandler::Lookup*ByName. Never call those:
// every one of them does strlen(file->filename) on each container entry
// without checking it, and TESDataHandler::files (walked by LookupModByName,
// hence by LookupForm) is a BSSimpleList whose embedded head node can hold a
// null value while still linking to a tail. BSSimpleList::empty() only reports
// empty when BOTH value and next are null, so such a list iterates and hands
// out that null on the very first step -> strlen((char*)0x70) -> access
// violation. See BNSbug-2026-07-31-null-tesfile.md.
const RE::TESFile* FindLoadedMod(RE::TESDataHandler& a_dataHandler, const std::string& a_modName)
{
    const auto matches = [&a_modName](const RE::TESFile* a_file) {
        return a_file && a_modName.size() == std::strlen(a_file->filename)
          && _strnicmp(a_file->filename, a_modName.data(), a_modName.size()) == 0;
    };

    for (const auto* f : a_dataHandler.compiledFileCollection.smallFiles)
        if (matches(f)) return f;
    for (const auto* f : a_dataHandler.compiledFileCollection.files)
        if (matches(f)) return f;

    return nullptr;
}

// Tolerates both "ModName|FormID" and plain EditorID forms.
template <class T>
T* SmartLookup(std::uint32_t a_rawId, const std::string& a_modName)
{
    if (a_modName.empty() || a_rawId == 0) return nullptr;

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return nullptr;

    const auto* file = FindLoadedMod(*dataHandler, a_modName);
    if (!file)
    {
        spdlog::warn("Mod '{}' is not loaded. Cannot resolve FormID {:X}", a_modName, a_rawId);
        return nullptr;
    }

    // The file we just found already carries its load-order index, so build the
    // runtime FormID here instead of looking the mod up a second time.
    const bool light = file->IsLight();
    const std::uint32_t localId = light ? (a_rawId & 0xFFFu) : (a_rawId & 0xFFFFFFu);
    const std::uint32_t formID = light
      ? (0xFE000000u | (static_cast<std::uint32_t>(file->smallFileCompileIndex) << 12) | localId)
      : ((static_cast<std::uint32_t>(file->compileIndex) << 24) | localId);

    auto* form = RE::TESForm::GetFormByID(formID);
    if (!form)
    {
        spdlog::warn("FormID {:X} (Local: {:X}) not found in Mod '{}'", formID, localId, a_modName);
        return nullptr;
    }
    return form->As<T>();
}

template <class T>
bool SmartTryResolve(T*& a_outForm, const std::string& a_input, const std::string& a_ruleName, const std::string& a_sourceFile)
{
    if (a_input.empty()) return true;

    auto pipePos = a_input.find('|');
    if (pipePos == std::string::npos)
    {
        // Plain EditorID
        auto* form = RE::TESForm::GetFormByEditorID(RE::BSFixedString(a_input));
        if (!form)
        {
            spdlog::warn("[{}] EditorID '{}' not found in loaded game data. Rule: '{}'", a_sourceFile, a_input, a_ruleName);
            return false;
        }
        a_outForm = form->As<T>();
        if (!a_outForm)
        {
            spdlog::warn("[{}] EditorID '{}' was found, but it is the WRONG TYPE. Rule: '{}'",
                         a_sourceFile,
                         a_input,
                         a_ruleName);
            return false;
        }
        return true;
    }

    // ModName|HexID
    std::string modName = a_input.substr(0, pipePos);
    std::string hexStr = a_input.substr(pipePos + 1);

    const char* parseStart = hexStr.data();
    if (hexStr.length() > 2 && hexStr[0] == '0' && (hexStr[1] == 'x' || hexStr[1] == 'X')) { parseStart += 2; }

    std::uint32_t formID = 0;
    auto [ptr, ec] = std::from_chars(parseStart, hexStr.data() + hexStr.size(), formID, 16);
    if (ec != std::errc())
    {
        spdlog::warn("[{}] Invalid FormID hex format '{}' in rule '{}'", a_sourceFile, a_input, a_ruleName);
        return false;
    }

    a_outForm = SmartLookup<T>(formID, modName);
    return a_outForm != nullptr;
}

// Resolve one rule. Returns nullopt to discard.
std::optional<SwapRuleResolved> ResolveRule(const SwapRule& a_rule)
{
    SwapRuleResolved res;
    res.sourceFile = a_rule.sourceFile;
    res.ruleName = a_rule.ruleName;
    res.debugLevel = a_rule.debugLevel;
    res.sortOrder = a_rule.sortOrder;

    // --- replaceBy (fatal) ---
    if (!a_rule.replaceByStr.empty())
    {
        if (!SmartTryResolve(res.replaceBy, a_rule.replaceByStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::error("[{} | '{}'] replaceBy form not found. Rule DISCARDED", a_rule.sourceFile, a_rule.ruleName);
            return std::nullopt;
        }
    }

    // --- spawn (fatal) ---
    if (!a_rule.spawnStr.empty())
    {
        if (!SmartTryResolve(res.spawn, a_rule.spawnStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] spawnAlongside form not found. Rule DISCARDED.", a_rule.sourceFile, a_rule.ruleName);
            return std::nullopt;
        }
    }

    // --- Required core filters (fatal) ---
    if (!a_rule.requiredFactionStr.empty())
    {
        if (!SmartTryResolve(res.requiredFaction, a_rule.requiredFactionStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] DISCARDED: Required Faction could not be resolved.",
                         a_rule.sourceFile,
                         a_rule.ruleName);
            return std::nullopt;
        }
    }
    if (!a_rule.requiredRaceStr.empty())
    {
        if (!SmartTryResolve(res.requiredRace, a_rule.requiredRaceStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] DISCARDED: Required Race could not be resolved.", a_rule.sourceFile, a_rule.ruleName);
            return std::nullopt;
        }
    }
    if (!a_rule.requiredBaseStr.empty())
    {
        if (!SmartTryResolve(res.requiredBase, a_rule.requiredBaseStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] DISCARDED: Required Base could not be resolved.", a_rule.sourceFile, a_rule.ruleName);
            return std::nullopt;
        }
    }

    // --- Required keywords (fatal) ---
    for (const auto& kwStr : a_rule.requiredKeywordStrs)
    {
        RE::BGSKeyword* kw = nullptr;
        if (!SmartTryResolve(kw, kwStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] DISCARDED: Required Keyword '{}' could not be resolved.",
                         a_rule.sourceFile,
                         a_rule.ruleName,
                         kwStr);
            return std::nullopt;
        }
        if (kw) res.requiredKeywords.push_back(kw);
    }

    // --- Excluded keywords (non-fatal) ---
    for (const auto& kwStr : a_rule.excludedKeywordStrs)
    {
        RE::BGSKeyword* kw = nullptr;
        if (!SmartTryResolve(kw, kwStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::info("[{} | '{}'] Excluded Keyword '{}' could not be resolved (rule NOT discarded).",
                         a_rule.sourceFile,
                         a_rule.ruleName,
                         kwStr);
        }
        else if (kw) { res.excludedKeywords.push_back(kw); }
    }

    // --- Excluded bases (non-fatal) ---
    for (const auto& ebStr : a_rule.excludedBaseStrs)
    {
        RE::TESForm* form = nullptr;
        if (!SmartTryResolve(form, ebStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::info("[{} | '{}'] Excluded BaseID '{}' could not be resolved (rule NOT discarded).",
                         a_rule.sourceFile,
                         a_rule.ruleName,
                         ebStr);
        }
        else if (form) { res.excludedBases.push_back(form); }
    }

    // --- Name matching (normalize to lower case once) ---
    for (const auto& name : a_rule.nameMustContain)
        res.nameMustContain.push_back(ToLower(name));
    for (const auto& name : a_rule.nameMustNotContain)
        res.nameMustNotContain.push_back(ToLower(name));

    // --- Required locational (OR; fatal if all fail) ---
    if (!a_rule.locationalFilters.empty())
    {
        int successCount = 0;
        for (const auto& locStr : a_rule.locationalFilters)
        {
            RE::TESForm* form = nullptr;
            if (!SmartTryResolve(form, locStr, a_rule.ruleName, a_rule.sourceFile))
            {
                spdlog::info("[{} | '{}']: Location '{}' could not be resolved.",
                             a_rule.sourceFile,
                             a_rule.ruleName,
                             locStr);
            }
            else if (form)
            {
                ++successCount;
                if (auto* cell = form->As<RE::TESObjectCELL>()) { res.requiredCells.push_back(cell); }
                else if (auto* ws = form->As<RE::TESWorldSpace>())
                {
                    res.requiredWorldSpaces.push_back(ws);
                    if (ws->location) res.requiredLocations.push_back(ws->location);
                }
                else if (auto* loc = form->As<RE::BGSLocation>()) { res.requiredLocations.push_back(loc); }
                else
                {
                    spdlog::warn("[{}] EditorID '{}' exists but is NOT a Cell, WorldSpace, or Location.",
                                 a_rule.sourceFile,
                                 locStr);
                }
            }
        }
        if (successCount == 0)
        {
            spdlog::warn("[{} | '{}'] DISCARDED: Specified Locations all failed to resolve (Rule would trigger "
                         "nowhere).",
                         a_rule.sourceFile,
                         a_rule.ruleName);
            return std::nullopt;
        }
    }

    // --- Excluded locational (non-fatal) ---
    for (const auto& locStr : a_rule.locationalFiltersExcluded)
    {
        RE::TESForm* form = nullptr;
        if (!SmartTryResolve(form, locStr, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::info("[{} | '{}'] Excluded Location '{}' is missing (rule NOT discarded).",
                         a_rule.sourceFile,
                         a_rule.ruleName,
                         locStr);
        }
        else if (form)
        {
            if (auto* cell = form->As<RE::TESObjectCELL>()) { res.excludedCells.push_back(cell); }
            else if (auto* ws = form->As<RE::TESWorldSpace>())
            {
                res.excludedWorldSpaces.push_back(ws);
                if (ws->location) res.excludedLocations.push_back(ws->location);
            }
            else if (auto* loc = form->As<RE::BGSLocation>()) { res.excludedLocations.push_back(loc); }
            else
            {
                spdlog::warn("[{}] EditorID '{}' is valid but is NOT a Cell, WorldSpace, or Location.",
                             a_rule.sourceFile,
                             locStr);
            }
        }
    }

    // --- Actions & modifiers (non-fatal) ---
    for (const auto& str : a_rule.addFactionsStrs)
    {
        RE::TESFaction* fac = nullptr;
        if (!SmartTryResolve(fac, str, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] Cannot find Faction '{}'.", a_rule.sourceFile, a_rule.ruleName, str);
        }
        else if (fac) { res.addFactions.push_back(fac); }
    }
    for (const auto& str : a_rule.addItemsStrs)
    {
        RE::TESBoundObject* item = nullptr;
        if (!SmartTryResolve(item, str, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] Cannot find Item '{}'.", a_rule.sourceFile, a_rule.ruleName, str);
        }
        else if (item) { res.addItems.push_back(item); }
    }
    for (const auto& str : a_rule.addOMODsStrs)
    {
        RE::BGSMod::Attachment::Mod* omod = nullptr;
        if (!SmartTryResolve(omod, str, a_rule.ruleName, a_rule.sourceFile))
        {
            spdlog::warn("[{} | '{}'] Cannot find OMOD '{}'.", a_rule.sourceFile, a_rule.ruleName, str);
        }
        else if (omod) { res.addOMODs.push_back(omod); }
    }

    // --- Scalars ---
    res.powerArmorState = a_rule.powerArmorState;
    res.interiorState = a_rule.interiorState;
    res.minLevel = a_rule.minLevel;
    res.maxLevel = a_rule.maxLevel;
    res.chanceMin = std::min(a_rule.chanceMin, a_rule.chanceMax);
    res.chanceMax = std::max(a_rule.chanceMin, a_rule.chanceMax);
    res.ChanceScalingPerLevel = a_rule.ChanceScalingPerLevel;
    res.skipUniques = a_rule.skipUniques;
    res.skipEssentials = a_rule.skipEssentials;
    res.dryRun = a_rule.dryRun;
    res.OMODRandomizationPerItem = a_rule.OMODRandomizationPerItem;
    res.boostSpecial = a_rule.boostSpecial;
    res.specialBaseline = a_rule.specialBaseline;
    res.specialPerLevel = a_rule.specialPerLevel;
    res.specialMaxTotal = a_rule.specialMaxTotal;
    res.specialPerStatCap = a_rule.specialPerStatCap;

    // --- Sanity: rule must do *something* ---
    const bool hasAction = res.replaceBy || res.spawn || !res.addFactions.empty() || !res.addItems.empty()
      || !res.addOMODs.empty() || res.boostSpecial;
    if (!hasAction)
    {
        spdlog::error("[{} | '{}'] Rule has NO ACTIONS! Would do nothing, so discarding.",
                      a_rule.sourceFile,
                      a_rule.ruleName);
        return std::nullopt;
    }

    return res;
}

std::string FormStr(RE::TESForm* a_form)
{
    if (!a_form) return "<null>";
    return std::format("{}[{:08X}]", BNS::Utils::GetFormName(a_form), a_form->GetFormID());
}

template <typename T>
std::string FormList(const std::vector<T*>& a_items)
{
    std::string s;
    for (auto* item : a_items)
    {
        if (!s.empty()) s += ", ";
        s += FormStr(item);
    }
    return s;
}

std::string StringList(const std::vector<std::string>& a_v)
{
    std::string s;
    for (const auto& item : a_v)
    {
        if (!s.empty()) s += ", ";
        s += item;
    }
    return s;
}

const char* PowerArmorStr(PowerArmorCondition a_pa)
{
    switch (a_pa)
    {
    case PowerArmorCondition::kIgnore:
        return "Ignore";
    case PowerArmorCondition::kMustWear:
        return "MustWear";
    case PowerArmorCondition::kMustNotWear:
        return "MustNotWear";
    }
    return "?";
}

const char* InteriorStr(InteriorCondition a_ic)
{
    switch (a_ic)
    {
    case InteriorCondition::kIgnore:
        return "Ignore";
    case InteriorCondition::kMustBeInterior:
        return "MustBeInterior";
    case InteriorCondition::kMustBeExterior:
        return "MustBeExterior";
    }
    return "?";
}

// Prints every field that differs from its default.
void DumpRule(const SwapRuleResolved& r)
{
    spdlog::info("");
    spdlog::info("---- Rule '{}'  (source: {}, debugLevel={})", r.ruleName, r.sourceFile, r.debugLevel);
    if (r.sortOrder != 0.0) spdlog::info("        sortOrder: {}", r.sortOrder);

    // Actions
    if (r.replaceBy) spdlog::info("        replaceBy:        {}", FormStr(r.replaceBy));
    if (r.spawn) spdlog::info("        spawnAlongside:   {}", FormStr(r.spawn));

    // Filters
    if (r.requiredBase) spdlog::info("        requiredBase:     {}", FormStr(r.requiredBase));
    if (!r.excludedBases.empty()) spdlog::info("        excludedBases:    {}", FormList(r.excludedBases));
    if (r.requiredFaction) spdlog::info("        requiredFaction:  {}", FormStr(r.requiredFaction));
    if (r.requiredRace) spdlog::info("        requiredRace:     {}", FormStr(r.requiredRace));
    if (!r.requiredKeywords.empty()) spdlog::info("        requiredKeywords: {}", FormList(r.requiredKeywords));
    if (!r.excludedKeywords.empty()) spdlog::info("        excludedKeywords: {}", FormList(r.excludedKeywords));

    if (!r.nameMustContain.empty()) spdlog::info("        nameMustContain:    {}", StringList(r.nameMustContain));
    if (!r.nameMustNotContain.empty()) spdlog::info("        nameMustNotContain: {}", StringList(r.nameMustNotContain));
    if (r.powerArmorState != PowerArmorCondition::kIgnore)
        spdlog::info("        powerArmorState:  {}", PowerArmorStr(r.powerArmorState));
    if (r.interiorState != InteriorCondition::kIgnore)
        spdlog::info("        interiorState:    {}", InteriorStr(r.interiorState));

    // Locational
    if (!r.requiredCells.empty()) spdlog::info("        requiredCells:       {}", FormList(r.requiredCells));
    if (!r.requiredWorldSpaces.empty())
        spdlog::info("        requiredWorldSpaces: {}", FormList(r.requiredWorldSpaces));
    if (!r.requiredLocations.empty()) spdlog::info("        requiredLocations:   {}", FormList(r.requiredLocations));
    if (!r.excludedCells.empty()) spdlog::info("        excludedCells:       {}", FormList(r.excludedCells));
    if (!r.excludedWorldSpaces.empty())
        spdlog::info("        excludedWorldSpaces: {}", FormList(r.excludedWorldSpaces));
    if (!r.excludedLocations.empty()) spdlog::info("        excludedLocations:   {}", FormList(r.excludedLocations));

    // Skip flags (defaults: true)
    if (!r.skipUniques) spdlog::info("        skipUniques:    false");
    if (!r.skipEssentials) spdlog::info("        skipEssentials: false");
    if (r.dryRun) spdlog::info("        dryRun:         true");

    // Level + chance
    if (r.minLevel != 0) spdlog::info("        minLevel: {}", r.minLevel);
    if (r.maxLevel != std::numeric_limits<int>::max()) spdlog::info("        maxLevel: {}", r.maxLevel);
    if (r.chanceMin != 100.0 || r.chanceMax != 100.0 || r.ChanceScalingPerLevel != 0.0)
        spdlog::info("        chance:  min={}  max={}  scale/level={}", r.chanceMin, r.chanceMax, r.ChanceScalingPerLevel);

    // Modifications
    if (!r.addFactions.empty()) spdlog::info("        addFactions:      {}", FormList(r.addFactions));
    if (!r.addItems.empty()) spdlog::info("        addItems:         {}", FormList(r.addItems));
    if (!r.addOMODs.empty()) spdlog::info("        addOMODs:         {}", FormList(r.addOMODs));
    if (r.boostSpecial)
        spdlog::info("        boostSpecial:     baseline={}  perLevel={}  maxTotal={}  perStatCap={}",
                     r.specialBaseline,
                     r.specialPerLevel,
                     r.specialMaxTotal,
                     r.specialPerStatCap);
}

void DumpRulesAtDebugLevel1Plus(const std::vector<SwapRuleResolved>& a_rules)
{
    const auto count = std::ranges::count_if(a_rules, [](const auto& r) { return r.debugLevel >= 1; });
    if (count == 0) return;

    spdlog::info("");
    spdlog::info("==================================================================");
    spdlog::info(" Printout of resolved rules with debugLevel >= 1 ({} rule(s)):", count);
    spdlog::info("==================================================================");

    for (const auto& r : a_rules)
    {
        if (r.debugLevel >= 1) DumpRule(r);
    }

    spdlog::info("==================================================================");
    spdlog::info("");
}

// FNV-1a 64 over every behavior-affecting field. STABLE across runs — bumping
// the serialization version is the only safe way to change what's hashed,
// because rule hashes are stored in the co-save for per-(actor, rule) dedupe.
//
// Excluded on purpose: ruleName, sourceFile, debugLevel, sortOrder.
// (sortOrder doesn't change individual rule outcome; it changes the order of
//  the resolved list, captured in the fingerprint, not per-rule hash.)
std::uint64_t ComputeRuleHash(const SwapRuleResolved& r)
{
    using namespace BNS::Utils;
    auto h = kFNV1aBasis;

    // Use (pluginFilename, localFormID) so the hash is stable across load-order
    // changes (adding mods, reordering, etc.). Stored in the co-save.
    auto chainFormID = [&](const RE::TESForm* a_f) { h = FNV1aChainFormStable(h, a_f); };
    auto chainFormList = [&](const auto& a_vec) {
        h = FNV1aChain(h, static_cast<std::uint32_t>(a_vec.size()));
        for (const auto* f : a_vec)
            chainFormID(f);
    };

    chainFormID(r.replaceBy);
    chainFormID(r.spawn);

    chainFormID(r.requiredBase);
    chainFormList(r.excludedBases);
    chainFormID(r.requiredFaction);
    chainFormID(r.requiredRace);
    chainFormList(r.requiredKeywords);
    chainFormList(r.excludedKeywords);

    chainFormList(r.requiredCells);
    chainFormList(r.requiredWorldSpaces);
    chainFormList(r.requiredLocations);
    chainFormList(r.excludedCells);
    chainFormList(r.excludedWorldSpaces);
    chainFormList(r.excludedLocations);

    h = FNV1aChain(h, static_cast<std::uint32_t>(r.nameMustContain.size()));
    for (const auto& s : r.nameMustContain)
        h = FNV1aChainStr(h, s);
    h = FNV1aChain(h, static_cast<std::uint32_t>(r.nameMustNotContain.size()));
    for (const auto& s : r.nameMustNotContain)
        h = FNV1aChainStr(h, s);

    h = FNV1aChain(h, static_cast<std::int32_t>(r.powerArmorState));

    // Interior filter — chained ONLY when set, for the same co-save-stability
    // reason as the SPECIAL boost below: a rule that doesn't use it must hash
    // identically to pre-feature builds.
    if (r.interiorState != InteriorCondition::kIgnore) h = FNV1aChain(h, static_cast<std::int32_t>(r.interiorState));

    h = FNV1aChain(h, r.minLevel);
    h = FNV1aChain(h, r.maxLevel);
    h = FNV1aChain(h, r.chanceMin);
    h = FNV1aChain(h, r.chanceMax);
    h = FNV1aChain(h, r.ChanceScalingPerLevel);
    h = FNV1aChain(h, r.skipUniques);
    h = FNV1aChain(h, r.skipEssentials);
    h = FNV1aChain(h, r.dryRun);
    h = FNV1aChain(h, r.OMODRandomizationPerItem);

    // SPECIAL boost — chained ONLY when active, so every rule that doesn't use
    // it hashes identically to pre-feature builds. This keeps existing co-save
    // dedupe entries valid (no serialization version bump / re-apply storm).
    if (r.boostSpecial)
    {
        h = FNV1aChain(h, r.specialBaseline);
        h = FNV1aChain(h, r.specialPerLevel);
        h = FNV1aChain(h, r.specialMaxTotal);
        h = FNV1aChain(h, r.specialPerStatCap);
    }

    chainFormList(r.addFactions);
    chainFormList(r.addItems);
    chainFormList(r.addOMODs);

    return h;
}

} // namespace

std::vector<SwapRuleResolved> ResolveRules(const std::vector<SwapRule>& a_rules)
{
    std::vector<SwapRuleResolved> result;
    result.reserve(a_rules.size());

    for (const auto& r : a_rules)
    {
        if (auto resolved = ResolveRule(r)) { result.push_back(std::move(*resolved)); }
        else
        {
            spdlog::error("===================================================================");
            spdlog::error(" RULE DISCARDED -> File: {} | Rule: {}", r.sourceFile, r.ruleName);
            spdlog::error("===================================================================");
        }
    }

    // Keep alphabetical sort of files, unless sortOrder is different.
    // This sort determines the order rules are evaluated against actors at runtime.
    std::stable_sort(result.begin(), result.end(), [](const SwapRuleResolved& a, const SwapRuleResolved& b) {
        return a.sortOrder < b.sortOrder;
    });

    // Compute per-rule hash after sort (sort doesn't affect individual hashes,
    // but we want the resolved list stable before the fingerprint downstream).
    for (auto& r : result)
        r.hash = ComputeRuleHash(r);

    DumpRulesAtDebugLevel1Plus(result);

    return result;
}
