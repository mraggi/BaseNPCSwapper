# BaseNPCSwapper — technical notes

How this works, what was reverse-engineered to make it work, and what was
learned the hard way. If you are just installing the mod, the
[README](../README.md) is all you need.

## The problem: Creation Engine is asynchronous about actor loading

Bethesda's Creation Engine overlaps actor 3D loading with face-gen, Havok
physics setup, and AI initialization. Mutating an actor's base form (`TESNPC`)
at the wrong moment causes T-poses, missing skeletons, race-mismatch crashes,
or silent CTDs. Almost everything below exists to get the timing right, not
because the swap itself is conceptually hard.

## Entry point: a vtable hook, not a Papyrus event

There is no vanilla event for "an actor's 3D just loaded." BNS installs its
own hook on `Actor::Load3D`'s vtable slot (`RE::VTABLE::Actor[0]`, vfunc index
`0x86`) at `kGameDataReady`, and every actor load — from world placement,
spawn, or cell transition — routes through it into a continuation-passing
pipeline (`NPCManager::QueueDeferredEvaluation`).

The pipeline hops between a single background timer thread
(`DelayManager`, used for polling) and the main thread
(`F4SE::GetTaskInterface()`) — no additional worker threads. `refID` is stable
throughout a chain; only the base form changes after a swap, so later steps
re-evaluate against the actor's new identity and re-fetch by `FormID` every
time, to handle despawn/death mid-chain.

```
Actor::Load3D hook
   └─► QueueDeferredEvaluation
       └─► Stabilize: poll Get3D() && !faceGenLoadPending
           └─► EvaluateRuleAt(refID, 0)
               │  match  → ExecuteMatchingRule → next rule
               │  no match → next rule
               │  end of list → FinalizeActor
```

## Race resolution: the fallback trap

Asking a `TESLevCharacter` (leveled-list "race") — or any not-yet-resolved
template chain — for its race returns `SynthGen2RaceValentine`, the last entry
in the race enum, which the engine uses as its "unresolved" fallback. It is
**not** the race the actor will end up with, and if it leaks through to a
rendered actor the symptom is an invisible body with a gen-2-synth head (Gen 2
Synths have a head mesh but no body mesh).

This turns out to be the single most useful diagnostic in the whole plugin:
`actor->race->GetFormID() == 0x002261A4` is both "the correctly-resolved
signal actor rendering depends on" and "the one authoritative check for a
bailed template resolution" — a templated NPC's chain (leveled-list templates,
nested SPECIAL/traits/factions slots) doesn't fully settle in one Disable→Enable
cycle, and this check catches every flavor of that failure without needing to
walk the template-slot bits directly (an earlier version did that, and it
false-positived on every healthy actor that legitimately keeps a non-Traits
template bit set — race is the field that actually decides whether the actor
renders, so checking anything else is both slower and wrong).

## Why the swap needs 9 phases

`PerformSwap` rolls the rule's `replaceBy` (resolving a leveled list if
necessary) to a concrete NPC, acquires a per-base face-gen lock (two actors
swapping to the *same* target NPC in parallel corrupts the engine's shared
face-gen geometry handle otherwise — the second one queues behind the first),
and then:

| Phase | What happens |
|------:|--------------|
| 1 | Disable, release 3D, clear face/biped process fields, strip stale extras. |
| 2 | Poll until 3D is actually gone — confirms teardown took effect. |
| 3 | "Surgery": `SetObjectReference(newBase)`, re-apply race (the engine's own setter clobbers it for templated hubs), re-inject original loot and a fully-resolved template block. |
| 4 | Reposition, re-enable. Engine starts instantiating the new identity. |
| 5 | Poll until the engine has settled on the new identity. |
| 6 | A second Disable→Enable cycle — see below for why this exists at all. |
| 7 | Poll again. |
| 8 | Restore worn-item defaults, reset HP via a Papyrus callback. |
| 9 | Final reposition, release the per-base lock, fire completion. |

**Phases 6/7 exist because nothing else clears a post-swap T-pose.**
`Reset3D`, `ResetHavokPhysics`, and `HandleDefaultAnimationSwitch` were all
tried, individually and combined — none of them stand the actor up. The only
thing that reliably works is a second full Papyrus Disable→Enable cycle, which
is what actually saving and reloading the game does natively; this simulates
that as closely as possible mid-session. The cost is that Disable→Enable also
re-rolls the template chain from scratch (the engine has no "keep the
previous roll" flag), so each cycle is a fresh chance to land on a degenerate
template resolution — which is exactly what the race-fallback check above
exists to catch and retry.

## EditorID resolution

Rules can reference forms by EditorID, but the engine only retains EditorIDs
for a handful of form types by default — most (NPCs included) are discarded
after the CK/xEdit-only lookup table is torn down. [Hydra] restores the full
table and is the recommended dependency; if it's absent, BNS installs its own
fallback (`EditorIDLoader`, adapted from Baka Framework, GPL-3.0) that hooks
`TESForm::GetFormEditorID`/`SetFormEditorID` (vfunc indices `0x3A`/`0x3B`) on
~90 form types and maintains its own `FormID → EditorID` map. It doesn't cover
everything Hydra does, which is why the plugin prompts at startup if Hydra
isn't detected.

[Hydra]: https://www.nexusmods.com/fallout4/mods/104159

## Attaching OMODs without a form list

`addOMODs` needs to know which OMOD attaches to which weapon/armor, and
CommonLibF4 doesn't expose an OMOD's `MNAM` (attach-point keyword) structurally
— so BNS parses it directly from the raw ESP/ESM bytes at startup, off the
main thread, only for OMODs actually referenced by a loaded rule. Power Armor
pieces are matched via that same MNAM data (their `APPR` records list frame
slots rather than mod slots); regular weapons/armor are matched by
attach-parent instead.

Attachment itself is batched through a single Papyrus call
(`BNSSpawnHelper.SafeAttachMods`, up to 8 OMODs per call) that does
drop → attach → re-add → re-equip in one VM turn — an earlier per-OMOD version
raced with the engine's own outfit reconciliation and frequently left items
un-equipped.

## Safety guarantees

- **Quest integrity** — the swap uses `SetObjectReference`, which keeps the
  original `ObjectReference`. Quest aliases, attached scripts, dialogue, and
  the reference's editor ID all survive; only the underlying base form
  changes.
- **No baked-in mid-swap state** — actors mid-pipeline are never serialized.
  A save taken during a polling gap just re-evaluates the actor on load.
- **Co-save resilience** — if a saved reference's plugin is no longer in the
  load order, F4SE's `ResolveFormID` returns nothing and the entry is quietly
  dropped rather than crashing.
- **Clean shutdown** — the hook checks a shutdown flag before queuing new
  work, and the uninstaller waits for in-flight chains to drain before
  signalling it's safe to remove the plugin.
