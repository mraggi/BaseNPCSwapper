ScriptName BNSSpawnHelper Native Hidden

;
; AttachModsToItem
;
; Atomically drops, attaches up to 8 OMODs, and re-adds one item. Does NOT
; equip — that's handled later by C++ after the engine has settled, via
; EquipItemSafe. Splitting the equip out is what fixes the "OMOD attaches
; but item ends up un-equipped" bug: equipping in the same VM turn as the
; drop/add races with the engine's outfit reconciliation; doing it on a
; later VM turn (post-drain) lets reconciliation finish first.
;
; The trailing BNSOnAttachComplete callback signals C++ that this item is
; ready to be re-equipped, and whether it was originally equipped.
;
; Slots 1..8 are FormIDs; 0 means "unused". The function tolerates any
; sparseness (e.g. only slot 1 used, or 1+3+5).
;
Function AttachModsToItem(Int refActor, Int refItemForm, Int refMod1, Int refMod2, Int refMod3, Int refMod4, Int refMod5, Int refMod6, Int refMod7, Int refMod8, Bool abWasEquipped) Global
    Actor akActor   = Game.GetForm(refActor) as Actor
    Form akItemForm = Game.GetForm(refItemForm)

    If !akActor || !akItemForm
        BNSOnAttachComplete(refActor, refItemForm, abWasEquipped, false)
        return
    EndIf

    ; Dropping converts to ObjectReference so AttachMod can be called.
    ObjectReference droppedItem = akActor.DropObject(akItemForm, 1)
    If !droppedItem
        BNSOnAttachComplete(refActor, refItemForm, abWasEquipped, false)
        return
    EndIf

    If refMod1 != 0
        ObjectMod akMod = Game.GetForm(refMod1) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf
    If refMod2 != 0
        ObjectMod akMod = Game.GetForm(refMod2) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf
    If refMod3 != 0
        ObjectMod akMod = Game.GetForm(refMod3) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf
    If refMod4 != 0
        ObjectMod akMod = Game.GetForm(refMod4) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf
    If refMod5 != 0
        ObjectMod akMod = Game.GetForm(refMod5) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf
    If refMod6 != 0
        ObjectMod akMod = Game.GetForm(refMod6) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf
    If refMod7 != 0
        ObjectMod akMod = Game.GetForm(refMod7) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf
    If refMod8 != 0
        ObjectMod akMod = Game.GetForm(refMod8) as ObjectMod
        If akMod
            droppedItem.AttachMod(akMod)
        EndIf
    EndIf

    akActor.AddItem(droppedItem, 1, true)

    BNSOnAttachComplete(refActor, refItemForm, abWasEquipped, true)
EndFunction


;
; EquipItemSafe
;
; Equips one item by base form and reports back whether the engine accepted
; it. Dispatched by C++ after the OMOD drain so that the engine's post-swap
; reconciliation can't un-equip what we just attached.
;
Function EquipItemSafe(Int refActor, Int refItemForm) Global
    Actor akActor   = Game.GetForm(refActor) as Actor
    Form akItemForm = Game.GetForm(refItemForm)

    Bool equipped = false
    If akActor && akItemForm
        akActor.EquipItem(akItemForm, false, true)
        equipped = akActor.IsEquipped(akItemForm)
    EndIf

    BNSOnEquipComplete(refActor, refItemForm, equipped)
EndFunction


; C++ bindings — Papyrus signals each phase's completion so the modifier
; pipeline can chain attach → drain → equip without timing guesses.
Bool Function BNSOnAttachComplete(Int refActor, Int refItemForm, Bool abWasEquipped, Bool abAttached) Global Native
Bool Function BNSOnEquipComplete(Int refActor, Int refItemForm, Bool abEquipped) Global Native


Function Spawn(Int originalRefID, Int newBaseID) Global
    ObjectReference originalRef = Game.GetForm(originalRefID) as ObjectReference
    Form newBase = Game.GetForm(newBaseID)

    If originalRef && newBase
        originalRef.PlaceAtMe(newBase, 1, false, false, false)
    EndIf
EndFunction


; Restores the actor's HP/limb state to a clean baseline after a base swap.
;
; v3 (2026-05-30): the Disable -> Enable that used to live here is GONE.
; Each Disable/Enable causes the engine to re-roll the actor's template
; chain (LVLN picks etc.), and a re-roll can land on a degenerate
; "Legendary Cataphract"-style entry that renders as a floating gen-2
; synth head. The C++ pipeline now does exactly ONE Disable/Enable (with
; a broken-resolution retry guard) and we don't want any more re-rolls
; after that. HP/limb reset alone is what this function should do.
;
; The trailing BNSResetActorStateDone callback is kept so the C++ pipeline
; can still await this (Step 17 only fires after the VM has actually
; returned from ResetHealthAndLimbs).
Function ResetActorState(Int refActor) Global
    Actor akActor = Game.GetForm(refActor) as Actor
    If akActor
        akActor.ResetHealthAndLimbs()
    EndIf
    BNSResetActorStateDone(refActor)
EndFunction

; C++ binding — fired by ResetActorState at end-of-call so the swap
; pipeline can resume Phase 9 only after the equip state has settled.
Bool Function BNSResetActorStateDone(Int refActor) Global Native


;
; ResetHealth
;
; Fire-and-forget polish at the end of a swap chain. Resets HP / limb
; damage / wound state to a clean baseline so the post-swap actor doesn't
; come back with whatever damage the previous base had.
;
; No callback — the swap pipeline doesn't gate anything on this.
Function ResetHealth(Int refActor) Global
    Actor akActor = Game.GetForm(refActor) as Actor
    If akActor
        akActor.ResetHealthAndLimbs()
    EndIf
EndFunction


; Awaited Disable: tail-call BNSOnPapyrusDisableDone so the C++ Phase-1
; chain only advances after the VM has actually run Disable(). Without
; this, large facegen pools (e.g. Diverse Wasteland) leave the engine
; mid-load when we tear 3D down, crashing the IOManager / BSJobs threads
; under multi-actor spawn waves.
Function PapyrusDisable(Int refActor) Global
    Actor akActor = Game.GetForm(refActor) as Actor
    If akActor
        akActor.Disable()
    EndIf
    BNSOnPapyrusDisableDone(refActor)
EndFunction


; Awaited Enable. Paired with PapyrusDisable — Phase 4 holds off on
; the 0xFF-leaf poll until the VM has actually run Enable().
Function PapyrusEnable(Int refActor) Global
    Actor akActor = Game.GetForm(refActor) as Actor
    If akActor
        akActor.Enable()
    EndIf
    BNSOnPapyrusEnableDone(refActor)
EndFunction


Bool Function BNSOnPapyrusDisableDone(Int refActor) Global Native
Bool Function BNSOnPapyrusEnableDone(Int refActor) Global Native


;
; DumpInfo
;
; Console diagnostic. Call from the in-game console:
;   cgf "BNSSpawnHelper.DumpInfo" <refID>
;
; The argument is taken as ObjectReference so the console can resolve the
; raw hex refID (e.g. ff003456) natively — passing it as Int makes the
; console treat the hex as an identifier and pass 0 instead.
;
; Logs the actor's base / race / 3D / ExtraLeveledCreature / inventory /
; equipped state / extra-data flags to BaseNPCSwapper.log at info level.
; Used to compare actor state across a Disable/Enable cycle so we can see
; exactly what the engine rebuilds vs preserves.
Function DumpInfo(ObjectReference target) Global
    If target
        BNSDumpInfo(target.GetFormID())
    Else
        BNSDumpInfo(0)
    EndIf
EndFunction

Bool Function BNSDumpInfo(Int refActor) Global Native


;
; RefreshActor
;
; Dispatched by NPCModifier::ApplyModificationsAsync after any OMOD work
; (in-place or Papyrus) finishes. The Disable -> Enable cycle is the
; reliable way to make the engine re-evaluate worn items so newly
; attached mods become visible / take effect. Same mechanism that fixes
; T-poses post-swap. Cheap to call; safe to skip when nothing changed.
;
Function RefreshActor(Int refActor) Global
    Actor akActor = Game.GetForm(refActor) as Actor
    If akActor
        akActor.Disable()
        akActor.Enable()
    EndIf
EndFunction
