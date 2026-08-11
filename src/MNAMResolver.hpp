#pragma once

#include "SwapRuleResolved.hpp"
#include <RE/Fallout.h>
#include <vector>

namespace BNS::MNAMResolver
{

// Build the FormID -> BGSKeyword* cache. Parses each unique source file
// referenced by the resolved rules, but only registers OMODs whose
// runtime FormID is actually used by at least one rule.
void BuildCache(const std::vector<SwapRuleResolved>& a_rules);

// Incremental rebuild: scans only the OMODs whose FormID is NOT already
// in the cache. Used by the Reload Rules flow — re-scanning every ESP/ESM
// on a large modlist is the slowest step of reload, so we skip it for any
// OMOD we've already seen this session. Existing entries are kept (cache
// is append-only; harmlessly grows past the current rule set).
void BuildCacheIncremental(const std::vector<SwapRuleResolved>& a_rules);

// Look up the MNAM keyword for an OMOD. Returns nullptr if the OMOD
// record has no MNAM, its file wasn't parsed, or the OMOD wasn't needed.
RE::BGSKeyword* GetMNAM(RE::BGSMod::Attachment::Mod* a_omod);
RE::BGSKeyword* GetMNAM(RE::TESFormID a_omodFormID);

// Cache readiness flag. Flips to true once BuildCache has run AND every
// rule's per-rule OMODCache has been patched. Callers that depend on OMOD
// metadata (NPCModifier::ApplyModificationsAsync) must defer until this
// returns true. Acquire/release semantics so a true return guarantees
// the writes inside BuildCache + per-rule patch are visible.
bool IsCacheReady();
void MarkCacheReady();

} // namespace BNS::MNAMResolver
