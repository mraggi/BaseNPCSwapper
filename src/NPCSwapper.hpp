#pragma once

#include "SwapRuleResolved.hpp"
#include <cstdint>
#include <functional>

namespace BNS::Swapper
{

// Entry point from NPCManager. a_onComplete fires exactly once when the
// swap chain ends (success, timeout, vanished actor, non-NPC LVLN roll,
// …). Re-fetch the actor by FormID inside the callback.
void PerformSwap(std::uint32_t a_refID, const SwapRuleResolved& a_rule, std::function<void()> a_onComplete);

// Papyrus -> C++ callback. main.cpp's BNSResetActorStateDone native binding
// forwards here when BNSSpawnHelper.ResetActorState finishes for an actor.
// Phase 8 parks a resume keyed on the refID; this fires it. Idempotent —
// only the first of {this, the Phase-8 timeout} wins the consume.
void OnResetActorStateDone(std::uint32_t a_refID);

// Phase 1 / Phase 4 Papyrus-await callbacks. Same single-slot pattern as
// OnResetActorStateDone. main.cpp binds BNSOnPapyrusDisableDone /
// BNSOnPapyrusEnableDone to these.
void OnPapyrusDisableDone(std::uint32_t a_refID);
void OnPapyrusEnableDone(std::uint32_t a_refID);

// Console diagnostic. Reachable via `cgf "BNSSpawnHelper.DumpInfo" <refID>`.
// Dumps base / race / 3D / ExtraLeveledCreature / inventory + equipped state /
// present extra-data types to the log at info level. Used to compare actor
// state across Disable/Enable cycles when investigating what the engine
// rebuilds vs preserves.
void DumpActorInfo(std::uint32_t a_refID);

} // namespace BNS::Swapper
