[b]Dynamically (F4SE) replace any NPC in the game at runtime using INI files. No leveled list conflicts, no (hopefully) broken quests.[/b]

This version works, theoretically, with NG+ (NG, AE, and the may 2026 update). But please test and report bugs. Tested extensively on AE. Help me test other versions!

One day I want to make it compatible with OG, but today is not that day.

Needs [url=https://www.nexusmods.com/fallout4/mods/42147]F4SE[/url], [url=https://www.nexusmods.com/fallout4/mods/47327]Address Library[/url], and optionally [url=https://www.nexusmods.com/fallout4/mods/104159]Hydra[/url] if your rules use EditorIDs (highly recommended).

[b][url=https://github.com/mraggi/BaseNPCSwapper]Github page[/url].[/b]

[b]⚠️ BETA STATUS WARNING ⚠️[/b]
This is a BETA release of a highly complex engine-level plugin. Might break a quest! If so, report a bug.

This started out as a pure swapper, but it's now a [b]full filter and modification system[/b] that can replace, spawn a bodyguard, or modify actors dynamically, with more features incoming.

[b][size=4]The Problem with Fallout 4 Leveled Lists[/size][/b]
If you've spent any time modding Fallout 4, you know that adding new enemies to the game is a nightmare. Unlike weapons, NPC leveled lists are an absolute tangled web of inheritance. If a mod author tries to dynamically inject a custom "Super Mutant War Machine", there is a high chance it will break a template or cause naked enemies to appear.

Because of this, mod authors usually bypass leveled lists entirely and place their custom enemies by hand. Take RascalArt's incredible [url=https://www.nexusmods.com/fallout4/mods/91724]Locked and Loaded[/url] mod. It adds some amazing Gunner robots. But because they are hand-placed, you will only see them where the author put them. When Gunners attack your settlement, those robots won't be there.

[b]The Solution[/b]
BaseNPCSwapper (BNS) is an F4SE plugin that intercepts the Creation Engine right as an NPC is loading their 3D skeleton. Before they even render on your screen, BNS reads your INI rules and seamlessly swaps their DNA, spawns an ally next to them, or injects items into their inventory.

Want to see RascalArt's robots during settlement attacks? Just write a simple INI rule telling BNS: "Whenever the game spawns a Gunner leveled 40-70, there is a 5% chance to spawn a Locked and Loaded Heavy Assaultron alongside them".

[b][size=4]Features & Safety (How it works)[/size][/b]
Replacing actors in Bethesda's engine is pretty dangerous. We took great pains to engineer this plugin to be as safe and robust as possible:

[list]
[*][b]Uniques & Essentials are safe[/b]: BNS automatically ignores any NPC marked as Unique or Essential by default (though you can override this in the INI).[/*]
[*][b]Quest Integrity (In-Place Swapping)[/b]: BNS does an "in-place" swap. The underlying Reference ID remains identical. If you have a Radiant Quest to "Clear out the Raiders," and you replaced some raiders by super mutants, killing the newly swapped Super Mutants will still properly progress the quest. Quest items are forwarded to the new body.[/*]
[*][b]Async Swap Pipeline[/b]: BNS disables the actor, swaps their base form, waits for the engine to finish generating the new face and stats, equips their gear, then re-enables them. A final polish cycle irons out any residual T-poses. The whole process is *mostly* invisible to the player.[/*]
[*][b]Save-Safe[/b]: Serialized directly to your .f4se co-save. If you quicksave right as an enemy is in the middle of being swapped, BNS will safely catch them and self-heal the process on reload.[/*]
[*][b]Dynamic Modification System[/b]: Don't want to swap the NPC? You can dynamically add them to new Factions, inject items into their inventory, or use `addOMODs` to safely attach weapon and armor modifications on the fly. BNS natively parses `.esp` files on startup to map exact `MNAM` targets, guaranteeing OMODs are only attached to valid weapons/armor.[/*]
[/list]

[b][size=4]A Simple Example[/size][/b]
Want to turn 50% of Radroaches into Molerats? Create a .ini file in Data/F4SE/Plugins/BaseNPCSwapper/ and write:

[code]{
    filterByRace = RadroachRace
    replaceBy = LCharMoleRat
    chance = 50
}[/code]

Or how about giving high-level Gunners in Quincy a sentry bot bodyguard?
[code]{
    filterByRace = Human
    filterByFaction = GunnerFaction
    filterByLocation = QuincyRuinsLocation
    spawnAlongside = LvlSentryBotGunner
    levelRange = 30~ // this means 30 or more
    chance = 10
}[/code]

(Check out the Articles tab for the full INI syntax guide!)

[b]⚠️ A Note on Factions Swaps[/b]
You will usually want to replace NPCs by other NPCs of the same faction. If you replace one Minuteman by a Deathclaw, it won't be a Minuteman Deathclaw; it will attack the other Minutemen. However, using BNS's new Modification System, you *could* technically add the `MinutemenFaction` to that Deathclaw dynamically! And it seems to work!
[code]{
    ruleName="Minutemen tamed deathclaws"
    filterByFaction = MinutemenFaction
    replaceBy = LCharDeathclaw
    addFactions = MinutemenFaction // makes the Deathclaw friendly
    chance=5 // means 5%
}[/code]

[b]Credits & Thanks[/b]
- Massive, endless gratitude to the F4SE Team and the contributors to CommonLibF4 / libxse.
- [url=https://www.nexusmods.com/fallout4/users/108352933]SoleVaultBoy[/url] for [url=https://www.nexusmods.com/fallout4/mods/104159]Hydra[/url] and [url=https://www.nexusmods.com/fallout4/users/5232181]shad0wshayd3[/url] for [url=https://www.nexusmods.com/fallout4/mods/43627]Baka Framework[/url]. Go and endorse them.
[url=https://www.nexusmods.com/fallout4/mods/69798][/url]- [url=https://next.nexusmods.com/profile/hoge111]hoge111[/url] for pointing out that EditorIDs are not in fact loaded by default.
- [url=https://next.nexusmods.com/profile/PTZar]PTZar[/url] for both suggesting Locational filtering AND detecting a facegen bug.
- [url=https://www.nexusmods.com/fallout4/users/8515543]Zzyxzz[/url] for [url=https://www.nexusmods.com/fallout4/mods/69798]RobCo patcher[/url] (syntax partly based on that mod).
