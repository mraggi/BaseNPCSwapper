#pragma once

#include "SwapRule.hpp"
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

// Main API
std::vector<SwapRule> ParseIniFiles(const std::filesystem::path& directory);
