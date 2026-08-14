[b][size=5]BaseNPCSwapper: Complete INI Syntax Guide[/size][/b]

BaseNPCSwapper (BNS) reads .ini files located in
[code]Data/F4SE/Plugins/BaseNPCSwapper/[/code]

You can have as many INI files as you want; BNS will read all of them alphabetically. You can also use the `sortOrder` key to force a specific priority (default: 0). Please place them in their own directory to avoid conflicts with other mods:
[code]Data/F4SE/Plugins/BaseNPCSwapper/MySupercoolMod/MySupercoolReplacer.ini[/code]

[b][size=4]1. The Basics & Syntax[/size][/b]
[list]
[*][b]Comments[/b]: You can write comments using // or ;. The plugin will ignore anything after these symbols on a line.[/*]
[*][b]Blocks[/b]: Every individual swap rule must be either enclosed in curly brackets { } OR on a single line, separated by :[/*]
[*][b]Keys/Values[/b]: Settings inside the block are written as `key = value`. Separate multiple values with commas. Some flag keys do not need a value.[/*]
[*][b]Quotes Don't Matter![/b] The parser automatically strips out quotation marks (`"` or `'`). Writing `filterByNameMustNotContain = "Boss"` is exactly the same as `filterByNameMustNotContain = Boss`. Use them preferably because it makes your INI easier for to read![/*]
[/list]

[b][size=4]2. Debugging Your Rules (CRUCIAL For Mod Authors!)[/size][/b]
If an NPC isn't getting modified and you don't know why, BNS has a highly detailed internal logging system.

The log file is always generated here:
[code]My Documents/My Games/Fallout4/F4SE/BaseNPCSwapper.log[/code]


To use it, you need to add two keys to your rule:
* [b]ruleName = "My Rule Name"[/b] (Gives your rule a readable name in the log so you can find it easily).
* [b]debugLevel = 0, 1, 2, 3, 4[/b]

[b]Debug Levels Explained:[/b]
[list]
[*][b]0 (Default):[/b] Completely silent. Nothing is written to the log for this rule.[/*]
[*][b]1 (Matches Only):[/b] On game load, prints the full resolved rule to the log so you can confirm it was parsed correctly. During gameplay, logs every time the rule successfully matches an actor, and prints a summary when modifications finish (e.g. `Done on 'Gunner' [Ref:A1B2C3]: 2 OMOD(s) attached, 1 item(s) added`).[/*]
[*][b]2 (Late Failures - Your Best Friend):[/b] Everything from level 1, plus logs actors who passed the broad filters (faction, race, location) but failed a late-stage check — keyword, power armor state, name, level range, or the random chance roll. Use this to find out why a specific actor isn't getting picked up or trace actors through the pipeline.[/*]
[*][b]3 (All Failures):[/b] Everything from level 2, plus the noisy early-exit failures (wrong faction, wrong race, outside location, unique/essential flag). Expect a lot of output. Use only when level 2 is not giving you enough.[/*]
[*][b]4 (Developer):[/b] Engine-level dumps at every pipeline step, Papyrus dispatch traces, and swap timing. Only useful if you are developing BNS itself.[/*]
[/list]

[b]⚠️ Note:[/b] Do NOT leave `debugLevel = 2`, `3`, or `4` on when releasing your mod! The engine evaluates hundreds of actors constantly; leaving this on will spam the user's log file. Level 1 is fine for released mods, and usually it's better to leave it at 0.

[b][size=4]3. Actions: Replace, Spawn, and/or Modify[/size][/b]
Your rule must define an action. You can completely replace the NPC by another, spawn a bodyguard next to them, or dynamically modify the NPC's inventory and factions (more modifications coming soon). You can use either the exact [b]EditorID[/b] (highly recommended for readability) OR the classic RobCo Patcher style [b]Mod.esp|FormID[/b] format.

[b]Action 1: Swap (Replace)[/b]
Performs an in-place swap, replacing the original NPC with your new target.
[list]
[*]Example: `replaceBy = LCharSuperMutant`[/*]
[/list]

[b]Action 2: Spawn Alongside (The "Bodyguard" Mode)[/b]
Spawns the new actor directly next to the target.
[list]
[*]Example: `spawnAlongside = EncDogAttackRaider`[/*]
[/list]
[b]⚠️ CRITICAL WARNING FOR SPAWNS:[/b] Be EXTREMELY careful with your filters when using spawn! If you tell the INI to spawn a Gunner next to every Gunner, the newly spawned Gunner will trigger the rule, spawning another, creating an infinite loop that will crash your game! ALWAYS use `chance` filters when spawning. See the last example.

[b]Action 3: Post-Spawn Modifications[/b]
Instead of replacing the NPC, you can dynamically modify them on the fly. You can combine these with `replaceBy` or `spawnAlongside`, or use them entirely on their own!
[list]
[*][b]addFactions[/b]: Injects the target into new factions. (e.g., `addFactions = MinutemenFaction`)[/*]
[*][b]addItems[/b]: Adds specific items/weapons to their inventory. (e.g., `addItems = Stimpack`)[/*]
[*][b]addOMODs / addOMODsToEquipment[/b]: Safely and dynamically attaches Object Modifications (OMODs) to the actor's weapons or armor. BNS reads the native `MNAM` data directly from your mod files to ensure the OMOD is only applied to valid weapons/armor, preventing engine crashes![/*]
[*][b]OMODRandomizationPerItem[/b]: `true` (default) or `false`. When true, the chance roll for adding OMODs is independent per item. When false, one roll decides for all items on the actor.[/*]
[/list]

[b]Testing without side effects: dryRun[/b]
Add `dryRun = true` to any rule to put it into testing mode. The rule will match and log exactly as normal (respecting `debugLevel`), but it will skip every mutation — no swap, no spawn, no faction/item/OMOD changes. The actor is also not marked as "already processed," so removing `dryRun` later will let the rule fire normally on the same actors. Use this to verify your filters are targeting the right NPCs before committing.

[b][size=4]4. Chances, Levels, and Priority[/size][/b]
[b]sortOrder = Number[/b]
* `sortOrder = -2.5` // Controls evaluation order. Higher numbers are evaluated last. Default is 0.

For example, if you swap a Raider to a Gunner first, THEN another rule evaluates if the actor is in the GunnerFaction, the second rule WILL fire. However, if you swap the order, since the raider is still a raider, the rule will not fire.

[b]levelRange = MinLevel~MaxLevel[/b]
* `levelRange = 10~50` // Only apply if the NPC is between level 10 and 50.
* `levelRange = 10~` // Only apply if the NPC is level 10 or above.

[b]chance = BaseChance~MaxChance~ScalingPerLevel[/b]
* [b]Flat Chance:[/b] `chance = 50` (A flat 50% chance).
* [b]Scaling Chance:[/b] `chance = 10~50~2.5` (At the minimum level, there is a 10% chance. Every level above the minimum adds 2.5%, capping at a maximum of 50%).

[b][size=4]5. Filters (AND vs OR Logic)[/size][/b]
[b]IMPORTANT LOGIC RULE:[/b] Almost every filter uses [b]AND[/b] logic. The ONLY exception is `filterByLocation`, which uses [b]OR[/b] logic (if the NPC is in Location A *OR* Location B, they get swapped).

[list]
[*][b]skipUniques[/b]: True by default. Set to `false` to allow modifying unique NPCs (careful!)[/*]
[*][b]skipEssentials[/b]: True by default. Set to `false` to allow modifying essential NPCs (careful!)[/*]
[*][b]filterByBaseID[/b]: Target a specific vanilla/modded NPC base.[/*]
[*][b]filterByBaseIDsExcluded[/b]: Do NOT target these specific base IDs.[/*]
[*][b]filterByFaction[/b]: Target anyone in a specific faction.[/*]
[*][b]filterByRace[/b]: Target anyone of a specific race.[/*]
[*][b]filterByKeywordsRequired[/b]: The NPC [b]must[/b] have all these keywords.[/*]
[*][b]filterByKeywordsExcluded[/b]: The NPC [b]must not[/b] have any of these keywords.[/*]
[*][b]filterByNameMustContain / MustNotContain[/b]: String checks on their display name. Case-insensitive.[/*]
[*][b]filterByLocation[/b]: The NPC must be in this Cell, WorldSpace, or Location.[/*]
[*][b]filterByLocationExcluded[/b]: Do not swap if they are in these locations. Higher priority than previous rule.[/*]
[*][b]filterByMustBeInterior[/b]: Only swaps actors indoors. No value needed — just the key on its own line. "Interior" is the Creation Kit's cell flag, so open-air settlements like Diamond City count as [i]exterior[/i].[/*]
[*][b]filterByMustBeExterior[/b]: The inverse — only swaps actors outdoors.[/*]
[*][b]filterByMustWearPowerArmor[/b]: Only swaps actors currently wearing a Power Armor frame.[/*]
[*][b]filterByMustNotWearPowerArmor[/b]: Only swaps actors walking around on foot.[/*]
[/list]

[b][size=5]Examples[/size][/b]

[b]Example 1: The Modern Locational Swap[/b]
Replace Triggermen with Synths, but ONLY if they are in Goodneighbor. 20% chance.
[code]{
    ruleName = "InstituteInfiltration"
    filterByFaction = TriggermanFaction
    filterByLocation = GoodneighborLocation
    chance = 20
    replaceBy = LCharSynth
}[/code]

[b]Example 2: The Bodyguard (Scaling Chance)[/b]
A Raider level 17+ might spawn an Attack Dog. Chance scales up to 20%.
[code]{
    ruleName = "RaiderDogBodyguard"
    filterByFaction = RaiderFaction
    levelRange = 17~
    chance = 1~20~0.5 // 1% at level 17, 1.5% at level 18, 2% at level 19 and so on, with a max chance of 20%
    spawnAlongside = EncDogAttackRaider
}[/code]

[b]Example 3: Dynamic Weapon Upgrades (The Modification System)[/b]
Paint half of gunner weapons with the creation club paint.
[code]{
    ruleName = "Gunner Paint"
    debugLevel = 1
    filterByFaction = GunnerFaction
    chance = 50
    addOMODs = ccgcafo4004-factionws04gun.esl|817 // creation club paint
}[/code]

[b]Example 4: Negative Name Filtering & Exclusions[/b]
Turn Raiders into Super Mutants, but exclude specific IDs and bosses.
[code]{
    ruleName = "RaidersToMutants_NoBosses"
    filterByFaction = RaiderFaction
    filterByNameMustNotContain = "Boss"
    filterByBaseIDsExcluded = EncRaider01Template
    chance = 5
    replaceBy = LCharSuperMutant
    addFactions = RaiderFaction
}[/code]

[b]Example 5: Double the feral ghouls (Math)[/b]
Half the time a feral ghoul spawns, another one spawns alongside it. Of course, when this new one spawns, it also might spawn other one and so on. On average, this duplicates the number of feral ghouls. If you want e.g. triple, solve the series equation (1+x+x²+... = 3 => 1/(1-x) = 3 => 1 = 3-3x => 3x=2 => x = 2/3, so set chance at 66.666) #mathisuseful

[code]{
    ruleName="Feral Ghoul Increase"
    filterByRace=FeralGhoulRace
    spawnAlongside=LCharFeralGhoul
    chance=50 // This, on average, gets you twice the number of ghouls, because 1+1/2+1/4+1/8+... = 2.
}[/code]
