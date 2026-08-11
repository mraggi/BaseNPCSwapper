ScriptName BNSUninstaller Native Hidden

; =============================================================================
; UNINSTALL FLOW
; =============================================================================

; 1. The native C++ bind
Bool Function UninstallBNS() Global Native

; 2. Call this from MCM
Function StartUninstall() Global
    Debug.Notification("BaseNPCSwapper: Uninstall initiated. Please wait...")
    UninstallBNS() ; Tells C++ to seal the hook and start draining the queue
EndFunction

; 3. C++ will dispatch this automatically when the queue hits 0
Function OnUninstallComplete() Global
    String uninstallMsg = "BaseNPCSwapper: Pipeline Empty and Hook Sealed!\n\n"
    uninstallMsg += "It is now 100% safe to uninstall the mod. Please follow these precise steps:\n\n"
    uninstallMsg += "1. Create a NEW manual save right now (Do not QuickSave).\n"
    uninstallMsg += "2. Quit completely to the desktop.\n"
    uninstallMsg += "3. Disable or remove BaseNPCSwapper in your Mod Manager.\n"
    uninstallMsg += "4. Launch the game and load your new save."

    Debug.MessageBox(uninstallMsg)
EndFunction


; =============================================================================
; PROCESSED-ACTOR CACHE FLOW
; =============================================================================
;
; The plugin remembers which actors it has already evaluated, so each actor is
; only processed once per save. Useful for stability, but a pain when iterating
; on rules: edits don't apply to actors that have already been "seen".
;
; This flow wipes that cache so every visible actor gets re-evaluated next
; time it's loaded (typically on the next cell transition, or sooner if the
; engine reuses the actor).

; 1. The native C++ bind
Bool Function ClearBNSCache() Global Native

; 2. Call this from MCM/Holotape to wipe the processed-actors set
Function StartClearCache() Global
    Bool ok = ClearBNSCache()
    If ok
        Debug.Notification("BaseNPCSwapper: Processed-actors cache cleared.")
    Else
        Debug.Notification("BaseNPCSwapper: Cache clear refused (uninstall in progress).")
    EndIf
EndFunction


; =============================================================================
; RELOAD RULES FLOW (dev-loop quality-of-life)
; =============================================================================
;
; Lets a modder edit INI files and pick up the changes without quit/restart:
;   1. Gates the C++ Load3D hook so no new actors enter the pipeline.
;   2. Drains in-flight swaps (with a 30s wall-clock cap).
;   3. Re-parses every INI in Data\F4SE\Plugins\BaseNPCSwapper\.
;   4. Incremental MNAM scan: only ESPs/ESMs containing OMODs not already
;      cached are re-scanned. The full scan can take seconds on a large
;      modlist; the incremental path is typically <100ms.
;   5. Rebuilds the per-rule OMOD index and atomically swaps in the new
;      rule list. Already-processed actors are cleared from the session
;      set so every live actor re-evaluates on its next Load3D.
;   6. C++ fires OnReloadComplete with the new rule count.

; 1. The native C++ bind
Bool Function BNSReloadRules() Global Native

; 2. Call this from MCM
Function StartReloadRules() Global
    Bool ok = BNSReloadRules()
    If ok
        Debug.Notification("BaseNPCSwapper: Reload started...")
    Else
        Debug.Notification("BaseNPCSwapper: Reload refused (uninstall or another reload in progress).")
    EndIf
EndFunction

; 3. C++ dispatches this when the reload finishes
Function OnReloadComplete(Int ruleCount) Global
    String msg = "BaseNPCSwapper: Reload complete. " + ruleCount + " rule(s) active."
    Debug.Notification(msg)
EndFunction
