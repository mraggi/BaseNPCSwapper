# BaseNPCSwapper (BNS) for Fallout 4

**[Available on Nexus Mods](https://www.nexusmods.com/fallout4/mods/104443)**

BaseNPCSwapper is an F4SE plugin that intercepts every NPC's 3D-load at runtime and, according to user-defined INI rules, can:

- **Replace** the NPC with a different base (`TESNPC` or rolled from an `LVLN`) — refID and quest aliases preserved.
- **Spawn** new actors alongside ("bodyguards").
- **Modify** the NPC in place: add factions, add inventory items, attach OMODs to weapons/armor (including Power Armor pieces).

Rules live in plain INI files under `Data/F4SE/Plugins/BaseNPCSwapper/`. There is no scripting required from the rule author.

---

## Table of contents

- [For mod authors: writing rules](#for-mod-authors-writing-rules)
- [For users: install + uninstall](#for-users-install--uninstall)
- [For developers: architecture](#for-developers-architecture)
- [Building](#building)
- [Credits](#credits)

---

## For mod authors: writing rules

Drop one or more `*.ini` files into `Data/F4SE/Plugins/BaseNPCSwapper/` (any subdirectory; BNS scans recursively, alphabetically). The full INI syntax reference lives in the [Nexus article](Documentation/NexusModsArticle.md) — what follows is the short version.

A rule is a curly-braced block of `key = value` pairs, or everything on a single line separated by `:`. Comments start with `//` or `;`.

```ini
{
    ruleName        = "Triggermen to Synths in Goodneighbor"
    filterByFaction = TriggermanFaction
    filterByLocation = GoodneighborLocation
    chance          = 20
    replaceBy       = LCharSynth
}
```

You can reference forms by **EditorID** (recommended; requires [Hydra](https://www.nexusmods.com/fallout4/mods/104159)) or by **`ModName.esp|FormID`**.

For a full in-depth explanation, see the [article on nexusmods](https://www.nexusmods.com/fallout4/articles/6494)

### What can a rule do?

| Key                 | Effect                                                                           |
|---------------------|----------------------------------------------------------------------------------|
| `replaceBy`         | Swap the NPC's base form to this NPC or rolled LVLN.                             |
| `spawnAlongside`    | Spawn this NPC at the target's position (the original is untouched).             |
| `addFactions`       | Comma-separated factions to add.                                                 |
| `addItems`          | Comma-separated items to add.                                                    |
| `addOMODs`          | Comma-separated OMODs to attach to matching weapons/armor (incl. Power Armor).   |
| `boostSpecialByLevel` | Raise the NPC's base SPECIAL toward a level-scaled target (fixes level-150/1-STR NPCs). |

Combine freely — a rule with `replaceBy` plus `addOMODs` swaps the base and then attaches mods to the new actor's gear.

**`boostSpecialByLevel`** raises base SPECIAL toward a target *total* that grows with the NPC's level, spreading the shortfall randomly across the 7 stats. It only ever raises a stat — NPCs already at or above the target are left alone — and the spread is deterministic per NPC. Formats:

- `boostSpecialByLevel` — bare, uses the defaults below.
- `boostSpecialByLevel = baseline~perLevel~maxTotal` — e.g. `10~0.25~50`.
- `boostSpecialByLevel = baseline~perLevel~maxTotal~perStatCap` — 4th field caps any single stat.

`target = clamp(baseline + perLevel × level, baseline, maxTotal)`. Defaults: `baseline 10`, `perLevel 0.25` (≈ +1 point per 4 levels), `maxTotal 50`, `perStatCap 10`. So a level-100 NPC targets 35 total SPECIAL. Note: NPCs flagged *auto-calc* derive stats from their class + level, so the write may not stick on them (a warning is logged at `debugLevel ≥ 1`).

### How rules pick their targets

Filter keys are AND-combined (the actor must satisfy every filter). The one exception is `filterByLocation`, which is OR (any listed cell/worldspace/location matches).

- **Identity**: `filterByBaseID`, `filterByBaseIDsExcluded`, `filterByFaction`, `filterByRace`
- **Keywords**: `filterByKeywordsRequired`, `filterByKeywordsExcluded`
- **Name**: `filterByNameMustContain`, `filterByNameMustNotContain` (substring, case-insensitive)
- **Location** (OR): `filterByLocation`, `filterByLocationExcluded`. Cells, worldspaces, and locations all accepted; location matching walks the `parentLoc` chain so child locations inherit parent matches.
- **Indoors/outdoors**: `filterByMustBeInterior` / `filterByMustBeExterior` (no value needed). Uses the cell's *interior* flag as the Creation Kit defines it, so enclosed-feeling exteriors like Diamond City count as exterior. Evaluated when the actor's 3D loads, in whatever cell that happens to be — an actor loaded outdoors that later walks inside is not re-checked.
- **State**: `filterByMustWearPowerArmor` / `filterByMustNotWearPowerArmor`
- **Defaults on**: `skipUniques`, `skipEssentials`. Set to `false` to override.
- **Level**: `levelRange = min~max` (open-ended on either side, e.g. `10~` or `~30`).
- **Chance**: `chance = flat` or `chance = min~max~scalingPerLevel` (the chance grows with the actor's level, clamped to `[min, max]`).
- **Priority**: `sortOrder` (lower runs earlier; default 0). Ties keep the alphabetical file order.

### Debugging

Set `debugLevel = N` in your rule and check `My Documents/My Games/Fallout4/F4SE/BaseNPCSwapper.log`:

- `0` — silent (default).
- `1` — log every successful match.
- `2` — also log failures *after* the early filters (faction/race/location). Useful when you know your target is reachable but the rule isn't firing.
- `3` — log every evaluation failure. Very noisy.
- `4` — full actor-state dumps at every phase transition. Reserved for reverse-engineering pipeline bugs.

**Don't ship a mod with `debugLevel >= 2`.** Every actor load is evaluated.

Rule *parsing* warnings are always logged, regardless of `debugLevel` — they happen once at load, before any rule has a debug level. The one to watch for is:

```
[MyRules.ini @ line 12] Unknown key 'filterByMustBeIndoors' — ignored. ...
```

A misspelled key is silently dropped, which for a **filter** means the rule ends up *less* restrictive than you intended — often firing on far more NPCs than you wanted. If a rule is matching things it shouldn't, check for this warning first. The same warning also appears when a value contains a `:`, because the parser treats `:` as a key separator (`ruleName = "Gunners: Phase 2"` parses as a rule named `"Gunners`).

---

## For users: install + uninstall

1. Install [F4SE](https://f4se.silverlock.org/) and [Hydra](https://www.nexusmods.com/fallout4/mods/104159).
2. [Download from Nexus Mods](https://www.nexusmods.com/fallout4/mods/104443)
3. Drop the contents of the release zip into your Fallout 4 install (or better yet - use a mod manager).
4. Add INI rules under `Data/F4SE/Plugins/BaseNPCSwapper/`.

**Game version:** built on CommonLibF4's OG/NG/AE support, so one DLL covers
the pre-Next-Gen ("OG", 1.10.163), Next-Gen ("NG") and Anniversary Edition
("AE") builds. You still need the [Address
Library](https://www.nexusmods.com/fallout4/mods/47327) entry matching your
exact exe version.

| Runtime | Status |
|---|---|
| **AE** (1.11.x) | **Tested.** Everything below is developed and verified here |
| **NG** (1.10.980+) | **Experimental** — believed correct, not confirmed in-game |
| **OG** (1.10.163) | **Experimental** — believed correct, not confirmed in-game |

OG and NG are experimental in the literal sense: the addresses come from
CommonLibF4's per-runtime tables and the code was checked against them, but
nobody has actually launched those builds with BNS installed. This project
develops on AE only. If BNS does nothing at all on OG or NG, or EditorID-based
rules fail to resolve, please report it — that's the feedback that would move
them out of experimental.

If Hydra isn't installed BNS will pop a MessageBox at launch — without Hydra, EditorID lookups for NPC-typed forms will fail and rules referencing those forms by EditorID may not work as intended. The `Mod.esp|FormID` syntax still works without Hydra.

### Uninstalling cleanly

The plugin ships with an MCM-driven uninstaller. Trigger `BNSUninstaller.UninstallBNS()` (via the MCM menu) before removing the plugin from your load order. It seals the Load3D hook, waits for any in-flight swap to finish, then signals the MCM script to clean up. Direct removal without the uninstaller is also safe — in-progress swaps are not serialized — but the uninstaller is the "polite" way. In fact, unless a swap is actively happening, it's exactly the same thing.

### MCM "Clear cache"

`BNSUninstaller.ClearBNSCache()` wipes the processed-actor set so every actor is re-evaluated against the rule. Useful when adding rules.

---

## For developers: architecture

Bethesda's Creation Engine is aggressively asynchronous and overlaps actor 3D loading with face-gen, Havok physics setup, and AI initialization. Mutating an actor's base form (`TESNPC`) at the wrong moment causes T-poses, missing skeletons, race-mismatch crashes, or silent CTDs. The bulk of BNS is shaped around getting the timing right.

### Lifecycle

1. **`kPostLoad`** — check Hydra; install the fallback `EditorIDLoader` if missing (based on Baka Framework).
2. **`kGameDataReady`** — parse INI files into `SwapRule`, resolve every string identifier to a live `RE::TESForm*` (`SwapRuleResolved`), hand the resolved list to `NPCManager`, install the `Actor::Load3D` VTable hook (index `0x86`), kick off a background OMOD-cache build on the `DelayManager` worker thread.
3. **Per actor** — the hook calls `NPCManager::QueueDeferredEvaluation`, which queues the actor on the manager's continuation-passing chain.

### The per-actor pipeline (`NPCManager`)

Each actor walks the rule list sequentially. The chain hops between `DelayManager` (a single background timer thread) and `F4SE::GetTaskInterface()` (the main thread) — no new worker threads are created beyond the one timer thread.

```
Actor::Load3D hook
   └─► NPCManager::QueueDeferredEvaluation
       └─► Stabilize: poll Get3D() && !faceGenLoadPending
           └─► EvaluateRuleAt(refID, 0)
               │   rule matches?
               │   ├─► ExecuteMatchingRule(refID, idx)
               │   │     1. PerformSpawnAlongside (fire-and-forget; spawned actor enters its own pipeline)
               │   │     2. Swapper::PerformSwap (9 phases; see below)
               │   │     3. Modifier::ApplyModificationsAsync (factions/items sync; OMODs via Papyrus)
               │   │     └─► EvaluateRuleAt(refID, idx + 1)
               │   no match
               │   └─► EvaluateRuleAt(refID, idx + 1)
               │   end of list
               │   └─► FinishActorPipeline → Swapper::FinalizeActor
```

The `refID` is stable throughout — only the base form changes after a swap, so the next rule re-evaluates against the actor's new identity. Every continuation re-fetches the actor by `FormID` to handle despawn / death mid-chain.

### The 9-phase swap (`NPCSwapper`)

Most of these are probably overcomplications, but whatever. It works...

`PerformSwap` first rolls the rule's `replaceBy` (LVLN if necessary) to a concrete NPC. It builds a `SwapContext`, acquires a per-base face-gen lock (so two actors swapping to the same NPC don't collide), and runs:

| Phase | What happens |
|------:|--------------|
| **1** | `Disable()`, `Release3DRelatedData()`, clear face/biped process fields, strip stale extras (RaceData, InstanceData, ModelSwap, PowerArmor*, OutfitItem, TextDisplayData). |
| **2** | Poll until `Get3D() == nullptr` — confirms teardown took effect. |
| **3** | "Surgery": gender twiddle on the shared `TESNPC`, pre-set race, call `SetObjectReference(newBase)`, re-apply race (the engine clobbers it inside the setter for templated hubs), inject the original loot, inject an `ExtraLeveledCreature` block with all 13 template slots resolved (LVLN rolls cached so shared LVLNs land on the same leaf). |
| **4** | `SetPosition`/`SetActorAngle`/`Enable(true)`. Engine starts instantiating the new identity. |
| **5** | Poll until "engine settled": templated hubs produce a `0xFF` leaf base; concrete bases never do, so we fall back to *(face-gen done && `Get3D() != nullptr`)*. |
| **6** | "Save/reload simulation": `Reset3D(reloadAll=true, queueReset=true)` + `ResetHavokPhysics()` + `HandleDefaultAnimationSwitch()`. Actually saving and reloading the game fixes T-poses; we can't trigger that mid-game, so this is the closest native approximation. |
| **7** | Poll until `Get3D() != nullptr` — rebuild completed. |
| **8** | Restore the shared-base gender flag, `InitDefaultWornImpl(true, true)`, dispatch `BNSSpawnHelper.ResetActorState` (Disable/Enable + HP reset). |
| **9** | Final `SetPosition`/`SetActorAngle`, release the per-base lock, fire the completion callback. |

The per-base face-gen lock matters when two actors hit the pipeline simultaneously with the same `replaceBy`: the second one's Phase 1 is queued in `g_pendingSwaps[targetBaseID]` and resumed when the first completes. Without this, the engine corrupts the shared face-gen geometry handle.

### Modifications (`NPCModifier`)

Runs after the swap chain finishes (or immediately, if a rule has no `replaceBy`).

- **Factions** — synchronous C++ writes to `ExtraFactionChanges` on the actor's `extraList`.
- **Items** — synchronous `AddObjectToContainer`.
- **OMODs** — batched via Papyrus. `BNSSpawnHelper.SafeAttachMods` takes up to 8 OMOD FormIDs per call and does drop → attach → re-add → re-equip in one atomic VM turn (the unbatched per-OMOD predecessor raced and frequently left items un-equipped). Candidates are filtered by the OMOD's attach-point keyword index *and* by `ma_*` recipe keywords on the target item. Power Armor pieces are matched via MNAM (their APPRs list frame slots, not mod slots); regular weapons/armor are matched via attach-parent.

The OMOD cache is built off-thread at boot (`MNAMResolver` reads raw ESP/ESM bytes since CommonLibF4 doesn't expose MNAM data structurally). Actors that hit the pipeline before the cache is ready get the rest of their rule applied normally; `ApplyModificationsAsync` re-queues itself on the `DelayManager` until `MNAMResolver::IsCacheReady()` returns `true`. No actors are lost.

### Other moving parts

- **`DelayManager`** — single background thread holding a sorted vector of `(executeTime, fn)` pairs; every callback hops back to the main thread via `F4SE::GetTaskInterface()->AddTask`. Polling uses the same primitive.
- **`EditorIDLoader`** — when Hydra is missing, hooks `GetFormEditorID` / `SetFormEditorID` (vfuncs `0x3A` / `0x3B`) on ~90 form types and maintains a `FormID → EditorID` side-map. Adapted from Baka Framework (GPL-3.0). Doesn't cover everything Hydra does, hence the prompt at startup.
- **`MNAMResolver`** — parses ESP/ESM headers on the worker thread, walks the OMOD group, and builds a `runtime FormID → MNAM keyword` map. Only OMODs referenced by at least one rule are parsed.
- **`serialization`** — F4SE co-save records the processed-actor set so reloading a save doesn't re-swap actors that have already been handled. Record type `'BNSA'`; V1 and V2 readers both supported.

### Source map

| File                              | Responsibility                                                              |
|-----------------------------------|-----------------------------------------------------------------------------|
| `main.cpp`                        | F4SE entry, hook install, Papyrus native bindings, message dispatch.        |
| `NPCManager.{hpp,cpp}`            | Per-actor rule walk and continuation chain.                                 |
| `NPCSwapper.{hpp,cpp}`            | The 9-phase swap, per-base lock, processed-actor set.                       |
| `NPCEvaluator.{hpp,cpp}`          | Rule matching + debug logging.                                              |
| `NPCModifier.{hpp,cpp}`           | Factions, items, OMOD batching.                                             |
| `IniParser.{hpp,cpp}`             | Parses INI files into `SwapRule`.                                           |
| `SwapRule.hpp`                    | String-based rule, straight from the INI.                                   |
| `SwapRuleResolved.{hpp,cpp}`      | Form-pointer rule + `ResolveRules` / `SmartTryResolve`.                     |
| `MNAMResolver.{hpp,cpp}`          | Raw ESP/ESM byte parser for OMOD MNAM keywords.                             |
| `DelayManager.{hpp,cpp}`          | Background timer thread → main-thread task dispatch.                        |
| `EditorIDLoader.hpp`              | Header-only fallback when Hydra is absent.                                  |
| `Utils.{hpp,cpp}`                 | LVLN rolls, template resolution, spawn helper, Papyrus dispatch, OMOD cache.|
| `serialization.{hpp,cpp}`         | F4SE co-save read/write.                                                    |
| `src/*.psc`                       | Papyrus side: `BNSSpawnHelper` (OMOD attach, enable/disable, HP reset), `BNSUninstaller` (MCM). |

All runtime C++ lives under `namespace BNS` with sub-namespaces `Swapper`, `Evaluator`, `Modifier`, `Utils`, `MNAMResolver`, `Serialization`, `EditorIDLoader`.

### Safety guarantees

- **Quest integrity** — `SetObjectReference` keeps the original `ObjectReference`. Quest aliases, attached scripts, dialogue, and `kACHR` all survive the swap. The actor in the world is the same actor; only the underlying NPC base form changes.
- **No baked-in mid-swap state** — actors in the in-progress set are intentionally NOT serialized. A save taken during a polling gap will simply re-evaluate the actor on load instead of persisting a headless / disabled record.
- **Co-save resilience** — when an entry references a plugin that's no longer in the load order, F4SE's `ResolveFormID` returns `nullopt` and we silently drop it.
- **Hook seal on shutdown** — the MCM uninstaller flips a flag the Load3D hook reads; new actor loads stop entering the pipeline immediately, then we wait for in-flight chains to drain.

---

### Dependencies

- **C++23** (`std::format`, `std::ranges`, designated initializers).
- **CommonLibF4** — git submodule at `lib/CommonLibF4`, the [Dear-Modding-FO4](https://github.com/Dear-Modding-FO4/commonlibf4) fork (adds OG/NG/AE multi-runtime support — see [docs/MultiRuntime.md](docs/MultiRuntime.md)).
- **spdlog**, **F4SE** — pulled in by the xmake plugin rule.

---

## Credits

- The libxse team, Dear-Modding-FO4, and every contributor to CommonLibF4 ([libxse/commonlibf4](https://github.com/libxse/commonlibf4), [Dear-Modding-FO4/commonlibf4](https://github.com/Dear-Modding-FO4/commonlibf4)). This plugin would be impossible without their reverse-engineering work.
- [Hydra](https://www.nexusmods.com/fallout4/mods/104159) for EditorID resolution.
- [Baka Framework](https://www.nexusmods.com/fallout4/mods/53872) — `EditorIDLoader` is derived from its loader (GPL-3.0; see `EXCEPTIONS.md`).
