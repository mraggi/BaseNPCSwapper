#include "IniParser.hpp"
#include <filesystem>
#include <iostream>
#include <vector>

// Simple helper to print vector strings
void PrintVector(const std::string& label, const std::vector<std::string>& vec)
{
    if (vec.empty()) return;
    std::cout << "    " << label << ": [";
    for (size_t i = 0; i < vec.size(); ++i)
    {
        std::cout << "\"" << vec[i] << "\"" << (i + 1 < vec.size() ? ", " : "");
    }
    std::cout << "]\n";
}

int main(int argc, char* argv[])
{
    // Default to current directory if no argument is passed
    std::filesystem::path targetDir = ".";
    if (argc > 1) { targetDir = argv[1]; }

    std::cout << "Scanning directory: " << std::filesystem::absolute(targetDir) << "\n";
    std::cout << "--------------------------------------------------\n";

    std::vector<SwapRule> rules = ParseIniFiles(targetDir);

    std::cout << "--------------------------------------------------\n";
    std::cout << "Found " << rules.size() << " total rules.\n";
    std::cout << "--------------------------------------------------\n\n";

    for (const auto& rule : rules)
    {
        std::cout << "=> Rule: '" << rule.ruleName << "' (" << rule.sourceFile << " @ Line " << rule.sourceLine << ")\n";
        std::cout << "    debugLevel: " << rule.debugLevel << "\n";
        std::cout << "    sortOrder:  " << rule.sortOrder << "\n";

        // Filters
        if (!rule.requiredBaseStr.empty()) std::cout << "    requiredBaseStr:    " << rule.requiredBaseStr << "\n";
        if (!rule.requiredFactionStr.empty())
            std::cout << "    requiredFactionStr: " << rule.requiredFactionStr << "\n";
        if (!rule.requiredRaceStr.empty()) std::cout << "    requiredRaceStr:    " << rule.requiredRaceStr << "\n";

        PrintVector("excludedBaseStrs", rule.excludedBaseStrs);
        PrintVector("requiredKeywordStrs", rule.requiredKeywordStrs);
        PrintVector("excludedKeywordStrs", rule.excludedKeywordStrs);
        PrintVector("nameMustContain", rule.nameMustContain);
        PrintVector("nameMustNotContain", rule.nameMustNotContain);
        PrintVector("locationalFilters", rule.locationalFilters);
        PrintVector("locationalFiltersExcluded", rule.locationalFiltersExcluded);

        std::cout << "    powerArmorState: " << static_cast<int>(rule.powerArmorState) << "\n";
        std::cout << "    levelRange:      " << rule.minLevel << " ~ " << rule.maxLevel << "\n";
        std::cout << "    chanceRange:     " << rule.chanceMin << " ~ " << rule.chanceMax
                  << " (Scaling: " << rule.ChanceScalingPerLevel << ")\n";
        std::cout << "    skipUniques:     " << (rule.skipUniques ? "true" : "false") << "\n";
        std::cout << "    skipEssentials:  " << (rule.skipEssentials ? "true" : "false") << "\n";

        // Actions
        if (!rule.replaceByStr.empty()) std::cout << "    replaceByStr: " << rule.replaceByStr << "\n";
        if (!rule.spawnStr.empty()) std::cout << "    spawnStr:     " << rule.spawnStr << "\n";

        // Post Mods
        PrintVector("addFactionsStrs", rule.addFactionsStrs);
        PrintVector("addItemsStrs", rule.addItemsStrs);
        PrintVector("addOMODsStrs", rule.addOMODsStrs);

        std::cout << "\n";
    }

    return 0;
}
