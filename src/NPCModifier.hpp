#pragma once

#include <RE/Fallout.h>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

struct SwapRuleResolved;

namespace BNS::Modifier
{

struct CachedOMOD
{
    RE::BGSMod::Attachment::Mod* mod = nullptr;
    std::vector<RE::BGSKeyword*> maKeywords;
};

// Two-way OMOD index:
//   byAttachPoint — keyed by OMOD attach-point kw idx; driven by item APPR
//                   (weapons / "traditional" armor).
//   byMNAM        — keyed by OMOD MNAM kw idx; driven by item keywords. The
//                   only path that matches PA pieces, whose APPRs describe
//                   frame slots (ap_PowerArmor_BodyMod, ap_Legendary), not
//                   mod slots. PA OMODs match via ma_* keywords instead.
struct OMODDictionary
{
    std::unordered_map<std::uint16_t, std::vector<CachedOMOD>> byAttachPoint;
    std::unordered_map<std::uint16_t, std::vector<CachedOMOD>> byMNAM;

    [[nodiscard]] bool empty() const noexcept { return byAttachPoint.empty() && byMNAM.empty(); }
};

// MNAMResolver must have been built before calling this.
OMODDictionary BuildOMODCache(const std::vector<RE::BGSMod::Attachment::Mod*>& a_omods);

// Fire-and-forget. Doesn't wait for Papyrus OMOD batches to drain — use
// ApplyModificationsAsync from pipeline contexts.
void ApplyModifications(RE::Actor* a_actor, const SwapRuleResolved& a_rule);

// Re-assert only the rule effects that are safe to run more than once, on an
// actor the pipeline already finished. Currently that is the SPECIAL boost
// alone: it's floor-to-target, so re-running either repairs a value the engine
// reverted (cell reset, auto-calc re-derivation, template re-resolve) or does
// nothing. It also re-reads the actor's *current* level, so a level-scaled
// target follows the actor instead of freezing at first-sighting level.
//
// Must run on the main thread. Never add items / OMODs / factions / swaps
// here — those are not idempotent and would duplicate on every cell load.
void ReapplyIdempotent(RE::Actor* a_actor, const SwapRuleResolved& a_rule);

// Pipeline entry. a_onComplete fires exactly once, after:
//   1. attach phase  — Papyrus drop+attach+re-add for every item with picks
//   2. drain         — fixed delay to let the engine settle
//   3. equip phase   — Papyrus EquipItemSafe per item the actor had equipped,
//                      gated on per-item BNSOnEquipComplete callbacks
// Vanished actors and no-op rules still fire it.
void ApplyModificationsAsync(std::uint32_t a_refID, const SwapRuleResolved& a_rule, std::function<void()> a_onComplete);

// Papyrus -> C++ callbacks (bound in main.cpp).
//
// OnAttachComplete: AttachModsToItem finished its drop+attach+re-add cycle
// for one (actor, item). If the item was originally equipped, the actor +
// item are queued for re-equip after the drain.
//
// OnEquipComplete: EquipItemSafe finished its EquipItem call. Decrements
// the per-actor in-flight count; when it hits zero, the modification
// pipeline advances to the next rule.
void OnAttachComplete(std::uint32_t a_refActor, std::uint32_t a_refItem, bool a_wasEquipped, bool a_attached);
void OnEquipComplete(std::uint32_t a_refActor, std::uint32_t a_refItem, bool a_equipped);

} // namespace BNS::Modifier
