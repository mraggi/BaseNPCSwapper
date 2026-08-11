# BaseNPCSwapper (BNS) for Fallout 4

> ### ⚠️ `experimental` branch
>
> This branch is the **multi-runtime port**, published for source review. It
> builds against a fork of CommonLibF4 that carries per-runtime addresses, so
> a single DLL targets OG (1.10.163), NG (up to 1.10.984) and AE (1.11.x)
> instead of NG/AE only.
>
> **It has not been run in-game at all** — development only has access to a
> Windows build VM, with no way to launch the game on this machine. OG/NG/AE
> address resolution is believed correct by inspection (see
> [docs/MultiRuntime.md](docs/MultiRuntime.md)), but two vtable hooks the
> plugin installs itself (`Actor::Load3D`, and the EditorID fallback) hardcode
> a slot index that is *assumed*, not confirmed, to be identical across all
> three runtimes. If that assumption is wrong on OG, the plugin silently does
> nothing rather than crashing. **Back up your save** if you try this on OG.
>
> `main` is the released, NG/AE-only code.

**[Available on Nexus Mods](https://www.nexusmods.com/fallout4/mods/104443)**

BaseNPCSwapper is an F4SE plugin that intercepts every NPC's 3D-load at runtime and, according to user-defined INI rules, can:

- **Replace** the NPC with a different base (`TESNPC` or rolled from an `LVLN`) — refID and quest aliases preserved.
- **Spawn** new actors alongside ("bodyguards").
- **Modify** the NPC in place: add factions, add inventory items, attach OMODs to weapons/armor (including Power Armor pieces).

Rules live in plain INI files under `Data/F4SE/Plugins/BaseNPCSwapper/`. There is no scripting required from the rule author.

An F4SE plugin: no ESP, no plugin slot. If you want the implementation details — the vtable hook, the 9-phase swap, the engine quirks that shaped it — see [docs/TECHNICAL.md](docs/TECHNICAL.md).

---

## Table of contents

- [For mod authors: writing rules](#for-mod-authors-writing-rules)
- [For users: install + uninstall](#for-users-install--uninstall)
- [Dependencies](#dependencies)
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
- **State**: `filterByMustWearPowerArmor` / `filterByMustNotWearPowerArmor`
- **Defaults on**: `skipUniques`, `skipEssentials`. Set to `false` to override.
- **Level**: `levelRange = min~max` (open-ended on either side, e.g. `10~` or `~30`).
- **Chance**: `chance = flat` or `chance = min~max~scalingPerLevel` (the chance grows with the actor's level, clamped to `[min, max]`).
- **Priority**: `sortOrder` (higher runs earlier; default 0).

### Debugging

Set `debugLevel = N` in your rule and check `My Documents/My Games/Fallout4/F4SE/BaseNPCSwapper.log`:

- `0` — silent (default).
- `1` — log every successful match.
- `2` — also log failures *after* the early filters (faction/race/location). Useful when you know your target is reachable but the rule isn't firing.
- `3` — log every evaluation failure. Very noisy.
- `4` — full actor-state dumps at every phase transition. Reserved for reverse-engineering pipeline bugs.

**Don't ship a mod with `debugLevel >= 2`.** Every actor load is evaluated.

---

## For users: install + uninstall

1. Install [F4SE](https://f4se.silverlock.org/) and [Hydra](https://www.nexusmods.com/fallout4/mods/104159).
2. [Download from Nexus Mods](https://www.nexusmods.com/fallout4/mods/104443)
3. Drop the contents of the release zip into your Fallout 4 install (or better yet - use a mod manager).
4. Add INI rules under `Data/F4SE/Plugins/BaseNPCSwapper/`.

**Game version:** this branch targets OG (pre-Next-Gen, 1.10.163), NG, and AE
from one DLL — you'll still need the [Address
Library](https://www.nexusmods.com/fallout4/mods/47327) entry matching your
exact exe version. **OG support is experimental**, see the warning at the top
of this file.

If Hydra isn't installed BNS will pop a MessageBox at launch — without Hydra, EditorID lookups for NPC-typed forms will fail and rules referencing those forms by EditorID may not work as intended. The `Mod.esp|FormID` syntax still works without Hydra.

### Uninstalling cleanly

The plugin ships with an MCM-driven uninstaller. Trigger `BNSUninstaller.UninstallBNS()` (via the MCM menu) before removing the plugin from your load order. It seals the Load3D hook, waits for any in-flight swap to finish, then signals the MCM script to clean up. Direct removal without the uninstaller is also safe — in-progress swaps are not serialized — but the uninstaller is the "polite" way. In fact, unless a swap is actively happening, it's exactly the same thing.

### MCM "Clear cache"

`BNSUninstaller.ClearBNSCache()` wipes the processed-actor set so every actor is re-evaluated against the rule. Useful when adding rules.

---

## Dependencies

- **C++23** (`std::format`, `std::ranges`, designated initializers).
- **CommonLibF4** — git submodule at `lib/CommonLibF4`, the [Dear-Modding-FO4](https://github.com/Dear-Modding-FO4/commonlibf4) fork (adds OG/NG/AE multi-runtime support — see [docs/MultiRuntime.md](docs/MultiRuntime.md)).
- **spdlog**, **F4SE** — pulled in by the xmake plugin rule.

---

## Credits

- The libxse team, Dear-Modding-FO4, and every contributor to CommonLibF4 ([libxse/commonlibf4](https://github.com/libxse/commonlibf4), [Dear-Modding-FO4/commonlibf4](https://github.com/Dear-Modding-FO4/commonlibf4)). This plugin would be impossible without their reverse-engineering work.
- [Hydra](https://www.nexusmods.com/fallout4/mods/104159) for EditorID resolution.
- [Baka Framework](https://www.nexusmods.com/fallout4/mods/53872) — `EditorIDLoader` is derived from its loader (GPL-3.0; see `EXCEPTIONS.md`).
