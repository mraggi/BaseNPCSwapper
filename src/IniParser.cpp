#include "IniParser.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <functional>
#include <ranges>
#include <spdlog/spdlog.h>
#include <string_view>
#include <unordered_map>

namespace fs = std::filesystem;

// --- Helper Functions --- //

template <typename T>
static T ParseNum(std::string_view str, T defaultVal = 0, int base = 10)
{
    if (str.empty()) return defaultVal;
    T val = defaultVal;
    std::from_chars(str.data(), str.data() + str.size(), val, base);
    return val;
}

static double ParseDouble(std::string_view str, double defaultVal = 0.0)
{
    if (str.empty()) return defaultVal;
    double val = defaultVal;
    std::from_chars(str.data(), str.data() + str.size(), val);
    return val;
}

static bool ParseBool(std::string_view str)
{
    if (str.empty()) return true; // valueless flag (e.g. "dryRun") → enabled
    if (str == "1" || str == "true" || str == "True" || str == "TRUE" || str == "yes" || str == "YES") return true;
    if (str == "0" || str == "false" || str == "False" || str == "FALSE" || str == "no" || str == "NO") return false;
    spdlog::warn("Unrecognized boolean value '{}' — treating as false. Use true/false (also 1/0, yes/no).", str);
    return false;
}

static void CleanString(std::string& s)
{
    if (s.empty()) return;

    // 1. Trim leading/trailing whitespace
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    auto lastNotSpace = s.find_last_not_of(" \t\r\n");
    if (lastNotSpace == std::string::npos)
    {
        s.clear();
        return;
    }
    s.erase(lastNotSpace + 1);

    // 2. Outermost quotes check
    if (s.length() >= 2)
    {
        if ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))
        {
            s = s.substr(1, s.length() - 2);
        }
    }
}

static void ParseStringList(std::string_view valStr, std::vector<std::string>& outVec)
{
    std::string currentToken;
    bool inQuotes = false;
    char quoteChar = '\0';

    for (size_t i = 0; i < valStr.size(); ++i)
    {
        char c = valStr[i];

        if (!inQuotes && (c == '"' || c == '\''))
        {
            // We entered a quoted substring
            inQuotes = true;
            quoteChar = c;
            // Retain the quote character for CleanString to strip later,
            // or omit it here if you want to strip it immediately.
            currentToken += c;
        }
        else if (inQuotes && c == quoteChar)
        {
            // We exited the quoted substring
            inQuotes = false;
            quoteChar = '\0';
            currentToken += c;
        }
        else if (c == ',' && !inQuotes)
        {
            // Found a comma OUTSIDE of quotes -> End of token
            CleanString(currentToken);
            if (!currentToken.empty()) { outVec.push_back(currentToken); }
            currentToken.clear();
        }
        else
        {
            // Normal character (or a comma/space safely protected inside quotes)
            currentToken += c;
        }
    }

    // Don't forget the final token after the last comma (or if there were no commas)
    CleanString(currentToken);
    if (!currentToken.empty()) { outVec.push_back(currentToken); }
}

// Shared parser for "chance" and the deprecated "replaceChance" alias.
// Format: min[~max[~scalingPerLevel]]
static void ApplyChanceField(SwapRule& r, std::string_view v)
{
    auto tildeSplit = v | std::views::split('~');
    auto it = tildeSplit.begin();
    if (it == tildeSplit.end()) return;

    r.chanceMin = ParseDouble(std::string_view {(*it).begin(), (*it).end()}, r.chanceMin);
    r.chanceMax = r.chanceMin;
    if (++it == tildeSplit.end()) return;

    r.chanceMax = ParseDouble(std::string_view {(*it).begin(), (*it).end()}, r.chanceMax);
    if (++it == tildeSplit.end()) return;

    r.ChanceScalingPerLevel = ParseDouble(std::string_view {(*it).begin(), (*it).end()}, r.ChanceScalingPerLevel);
}

// boostSpecialByLevel. Format: baseline~perLevel~maxTotal[~perStatCap].
// A bare key (no value) enables the boost with the shipped defaults.
static void ApplySpecialBoostField(SwapRule& r, std::string_view v)
{
    r.boostSpecial = true;
    if (v.empty()) return; // bare key → keep the defaults

    auto parts = v | std::views::split('~');
    auto it = parts.begin();
    const auto end = parts.end();
    auto tok = [&] { return std::string_view {(*it).begin(), (*it).end()}; };

    if (it == end) return;
    r.specialBaseline = static_cast<float>(ParseDouble(tok(), r.specialBaseline));
    if (++it == end) return;
    r.specialPerLevel = static_cast<float>(ParseDouble(tok(), r.specialPerLevel));
    if (++it == end) return;
    r.specialMaxTotal = static_cast<float>(ParseDouble(tok(), r.specialMaxTotal));
    if (++it == end) return;
    r.specialPerStatCap = ParseNum<int>(tok(), r.specialPerStatCap);
}

// --- Handlers for Key-Value Pairs --- //

using KeyHandler = std::function<void(SwapRule&, std::string)>;

static const std::unordered_map<std::string_view, KeyHandler> keyHandlers
  = {{"ruleName", [](SwapRule& r, std::string v) { r.ruleName = v; }},
     {"debugLevel", [](SwapRule& r, std::string v) { r.debugLevel = ParseNum<int>(v); }},
     {"sortOrder", [](SwapRule& r, std::string v) { r.sortOrder = ParseDouble(v); }},

     // --- Action Types & Targets ---
     {"replaceBy", [](SwapRule& r, std::string v) { r.replaceByStr = v; }},
     {"spawnAlongside", [](SwapRule& r, std::string v) { r.spawnStr = v; }},

     // Target Deprecations
     {"replaceByTarget",
      [](SwapRule& r, std::string v) {
          spdlog::warn("Deprecation Warning: 'replaceByTarget' is deprecated. Use 'replaceBy' instead.");
          r.replaceByStr = v;
      }},
     {"actionType",
      [](SwapRule& r, std::string v) {
          spdlog::warn("Deprecation Warning: 'actionType' is deprecated. Use 'spawnAlongside=TargetForm' instead.");
          std::ranges::transform(v, v.begin(), [](unsigned char c) { return std::tolower(c); });
          if (v == "spawn")
          {
              r.spawnStr = r.replaceByStr;
              r.replaceByStr = "";
          }
      }},
     {"targetMod",
      [](SwapRule& r, std::string v) {
          spdlog::warn("Deprecation Warning: 'targetMod' is deprecated. Use 'replaceBy' instead.");
          r.replaceByStr = v;
      }},

     // --- Broad Filters ---
     {"filterByFaction", [](SwapRule& r, std::string v) { r.requiredFactionStr = v; }},
     {"filterByRace", [](SwapRule& r, std::string v) { r.requiredRaceStr = v; }},
     {"filterByBaseID", [](SwapRule& r, std::string v) { r.requiredBaseStr = v; }},

     // --- Array Filters ---
     {"filterByKeywordsRequired", [](SwapRule& r, std::string v) { ParseStringList(v, r.requiredKeywordStrs); }},
     {"filterByKeywordsExcluded", [](SwapRule& r, std::string v) { ParseStringList(v, r.excludedKeywordStrs); }},
     {"filterByBaseIDsExcluded", [](SwapRule& r, std::string v) { ParseStringList(v, r.excludedBaseStrs); }},
     {"filterByNameMustContain", [](SwapRule& r, std::string v) { ParseStringList(v, r.nameMustContain); }},
     {"filterByNameMustNotContain", [](SwapRule& r, std::string v) { ParseStringList(v, r.nameMustNotContain); }},
     {"filterByLocation", [](SwapRule& r, std::string v) { ParseStringList(v, r.locationalFilters); }},
     {"filterByLocationExcluded", [](SwapRule& r, std::string v) { ParseStringList(v, r.locationalFiltersExcluded); }},

     // --- State Toggles (No value required) ---
     {"filterByMustWearPowerArmor", [](SwapRule& r, std::string) { r.powerArmorState = PowerArmorCondition::kMustWear; }},
     {"filterByMustBeInPowerArmor", [](SwapRule& r, std::string) { r.powerArmorState = PowerArmorCondition::kMustWear; }},
     {"filterByMustNotWearPowerArmor",
      [](SwapRule& r, std::string) { r.powerArmorState = PowerArmorCondition::kMustNotWear; }},
     {"filterByMustNotBeInPowerArmor",
      [](SwapRule& r, std::string) { r.powerArmorState = PowerArmorCondition::kMustNotWear; }},

     // --- Global Toggles ---
     {"skipUniques", [](SwapRule& r, std::string v) { r.skipUniques = ParseBool(v); }},
     {"skipEssentials", [](SwapRule& r, std::string v) { r.skipEssentials = ParseBool(v); }},
     {"OMODRandomizationPerItem", [](SwapRule& r, std::string v) { r.OMODRandomizationPerItem = ParseBool(v); }},
     {"dryRun", [](SwapRule& r, std::string v) { r.dryRun = ParseBool(v); }},

     // --- Post-Spawn Modifications ---
     {"addFactions", [](SwapRule& r, std::string v) { ParseStringList(v, r.addFactionsStrs); }},
     {"addItems", [](SwapRule& r, std::string v) { ParseStringList(v, r.addItemsStrs); }},
     {"addOMODs", [](SwapRule& r, std::string v) { ParseStringList(v, r.addOMODsStrs); }},
     {"addOMODsToEquipment", [](SwapRule& r, std::string v) { ParseStringList(v, r.addOMODsStrs); }},
     {"boostSpecialByLevel", [](SwapRule& r, std::string v) { ApplySpecialBoostField(r, v); }},

     // --- Complex Types (Ranges/Chances) ---
     {"chance", [](SwapRule& r, std::string v) { ApplyChanceField(r, v); }},
     {"replaceChance",
      [](SwapRule& r, std::string v) {
          spdlog::warn("Deprecation Warning: 'replaceChance' is deprecated. Use 'chance' instead.");
          ApplyChanceField(r, v);
      }},
     {"levelRange", [](SwapRule& r, std::string v) {
          auto tilde_pos = v.find('~');
          if (tilde_pos != std::string_view::npos)
          {
              std::string_view left = std::string_view(v).substr(0, tilde_pos);
              std::string_view right = std::string_view(v).substr(tilde_pos + 1);
              if (!left.empty()) r.minLevel = ParseNum<int>(left);
              if (!right.empty()) r.maxLevel = ParseNum<int>(right);
          }
      }}};

// --- Main Parser --- //

static void ParseRuleString(std::string_view ruleStr,
                            std::vector<SwapRule>& rules,
                            std::string_view filename,
                            size_t lineNum,
                            std::string_view /*originalLine*/)
{
    if (ruleStr.empty()) return;

    SwapRule rule;
    rule.sourceFile = std::string(filename);
    rule.sourceLine = lineNum;
    bool hasValidData = false;

    for (auto pairRange : ruleStr | std::views::split(':'))
    {
        std::string_view p {pairRange.begin(), pairRange.end()};
        if (p.empty()) continue;

        std::string keyStr;
        std::string valStr;
        auto eqPos = p.find('=');

        if (eqPos != std::string_view::npos)
        {
            keyStr = std::string(p.substr(0, eqPos));
            valStr = std::string(p.substr(eqPos + 1));
        }
        else
        {
            keyStr = std::string(p);
            valStr = ""; // Flawlessly handles valueless flags like filterByMustWearPowerArmor
        }

        CleanString(keyStr);
        valStr.erase(0, valStr.find_first_not_of(" \t\r\n"));
        valStr.erase(valStr.find_last_not_of(" \t\r\n") + 1);

        auto it = keyHandlers.find(keyStr);
        if (it != keyHandlers.end())
        {
            // For single string values (like ruleName), clean quotes here manually
            if (keyStr == "ruleName" || keyStr == "replaceBy" || keyStr == "spawnAlongside"
                || keyStr == "requiredFactionStr" || keyStr == "requiredRaceStr" || keyStr == "requiredBaseStr")
            {
                CleanString(valStr);
            }
            it->second(rule, valStr);
            hasValidData = true;
        }
    }

    if (hasValidData) rules.push_back(rule);
}

std::vector<SwapRule> ParseIniFiles(const fs::path& directory)
{
    std::vector<SwapRule> allRules;

    if (!fs::exists(directory) || !fs::is_directory(directory))
    {
        spdlog::debug("Target directory invalid: {}", directory.string());
        return allRules;
    }

    auto fileFilter = [](const fs::directory_entry& entry) {
        return entry.is_regular_file() && entry.path().extension() == ".ini";
    };

    auto iniFiles = fs::recursive_directory_iterator(directory) | std::views::filter(fileFilter)
      | std::views::transform([](auto&& e) { return e.path(); }) | std::ranges::to<std::vector>();

    std::ranges::sort(iniFiles, [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });

    for (const auto& filePath : iniFiles)
    {
        std::ifstream file(filePath);
        if (!file) continue;

        std::string originalLine;
        std::string blockBuffer;
        std::string currentFilename = filePath.filename().string();
        bool inBlock = false;
        size_t lineNum = 0;

        size_t blockStartLine = 0;
        std::string blockStartText;

        size_t rulesBefore = allRules.size();

        while (std::getline(file, originalLine))
        {
            lineNum++;
            std::string line = originalLine;

            auto commentPos = std::min(line.find("//"), line.find(";"));
            if (commentPos != std::string::npos) line.erase(commentPos);

            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            if (line.empty()) continue;

            for (char c : line)
            {
                if (c == '{')
                {
                    inBlock = true;
                    blockBuffer.clear();
                    blockStartLine = lineNum;
                    blockStartText = originalLine;
                }
                else if (c == '}')
                {
                    if (inBlock)
                    {
                        ParseRuleString(blockBuffer, allRules, currentFilename, blockStartLine, blockStartText);
                        blockBuffer.clear();
                        inBlock = false;
                    }
                }
                else
                {
                    blockBuffer += c;
                }
            }

            if (inBlock)
            {
                if (!blockBuffer.empty() && blockBuffer.back() != ':' && !line.empty()) { blockBuffer += ':'; }
            }
            else if (!blockBuffer.empty())
            {
                ParseRuleString(blockBuffer, allRules, currentFilename, lineNum, originalLine);
                blockBuffer.clear();
            }
        }

        size_t rulesParsedInFile = allRules.size() - rulesBefore;

        for (size_t i = rulesBefore; i < allRules.size(); ++i)
        {
            if (allRules[i].ruleName.empty()) { allRules[i].ruleName = std::format("Rule {}", i - rulesBefore + 1); }
        }

        spdlog::info("Parsed '{}' - Found {} rule(s).", currentFilename, rulesParsedInFile);
    }

    // NOTE: sorting moved to ResolveRules in SwapRuleResolved.cpp - the
    // authoritative sort is on resolved rules, not raw rules, and the rule
    // dump (debugLevel >= 1) happens right after that sort.

    return allRules;
}
