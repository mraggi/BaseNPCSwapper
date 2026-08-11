#pragma once

// =============================================================================
// EditorIDLoader.hpp
//
// Populates the engine's EditorID-to-Form map at form-load time, so
// RE::TESForm::LookupByEditorID(...) actually returns something for the form
// types whose EDIDs the engine would otherwise discard.
//
// HEAVILY ADAPTED FROM:
//   Baka Framework by shad0wshayd3-FO4
//   https://github.com/shad0wshayd3-FO4/BakaFramework
//   src/Patches/LoadEditorIDs.h
//   Licensed under GPL-3.0
//
// This file is offered under the same GPL-3.0 license as the original.
// Including this file in a project effectively requires that project (or at
// least the parts that link against this file) to be GPL-3.0 compatible.
// =============================================================================

#include <RE/Fallout.h>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace BNS::EditorIDLoader
{

namespace detail
{

    // Reverse map: formID -> EDID. Needed because for most form types
    // the engine's per-form data slot for EDID is zero-sized; the only
    // place the engine keeps EDIDs is the global map returned by
    // TESForm::GetAllFormsByEditorID(). When something calls
    // form->GetFormEditorID(), the default vfunc returns the per-form
    // slot, which is empty. We override that to consult our rmap.
    inline std::unordered_map<RE::TESFormID, std::string> g_edidMap;

    inline void AddToGameMap(RE::TESForm* a_this, const char* a_edid)
    {
        const auto& [map, lock] = RE::TESForm::GetAllFormsByEditorID();
        const RE::BSAutoWriteLock locker {lock.get()};
        if (!map) return;
        map->emplace(a_edid, a_this);
        g_edidMap.emplace(a_this->formID, a_edid);
    }

    template <class T>
    class Hook
    {
    public:
        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtable {T::VTABLE[0]};
            _GetFormEditorID = vtable.write_vfunc(0x3A, GetFormEditorID_Hook);
            _SetFormEditorID = vtable.write_vfunc(0x3B, SetFormEditorID_Hook);
        }

    private:
        static const char* GetFormEditorID_Hook(RE::TESForm* a_this)
        {
            auto it = g_edidMap.find(a_this->formID);
            if (it != g_edidMap.end()) return it->second.c_str();
            return _GetFormEditorID(a_this);
        }

        static bool SetFormEditorID_Hook(RE::TESForm* a_this, const char* a_edid)
        {
            if (a_edid && a_edid[0] != '\0' && a_this->formID < 0xFF000000) { AddToGameMap(a_this, a_edid); }
            return _SetFormEditorID(a_this, a_edid);
        }

        inline static REL::Relocation<decltype(&GetFormEditorID_Hook)> _GetFormEditorID;
        inline static REL::Relocation<decltype(&SetFormEditorID_Hook)> _SetFormEditorID;
    };

} // namespace detail

// -------------------------------------------------------------------------
// Detection: is an external EditorID loader already present?
// -------------------------------------------------------------------------
inline std::string DetectedLoader()
{
    if (GetModuleHandleA("Hydra.dll") != nullptr) return "Hydra";
    return "";
}

// -------------------------------------------------------------------------
// Sanity probe. Call after kGameDataReady, before reading rules.
//
// Returns true if a known-good vanilla form is resolvable by EditorID.
// "PlayerFaction" is a TESFaction in Fallout4.esm that always exists
// and is in a form type that none of the loaders skip.
//
// If this returns false, something is wrong - either the install didn't
// take, or both Baka/Hydra are present but misconfigured. Log an error
// and expect rule resolution to fail.
// -------------------------------------------------------------------------
inline bool IsWorking()
{

    // auto* ConcordLocation = RE::TESForm::GetFormByEditorID(RE::BSFixedString("ConcordLocation"));
    // auto* LCharDeathclaw = RE::TESForm::GetFormByEditorID(RE::BSFixedString("LCharDeathclaw"));
    // auto* LvlMinuteman = RE::TESForm::GetFormByEditorID(RE::BSFixedString("LvlMinuteman"));
    // auto* LvlDeathclaw = RE::TESForm::GetFormByEditorID(RE::BSFixedString("LvlDeathclaw"));
    //
    // if (!ConcordLocation)
    // 	spdlog::error("ConcordLocation not found!");
    //
    // if (!LCharDeathclaw)
    // 	spdlog::error("LCharDeathclaw not found!");
    //
    // if (!LvlMinuteman)
    // 	spdlog::error("LvlMinuteman not found!");

    return (RE::TESForm::GetFormByEditorID(RE::BSFixedString("LCharDeathclaw")) != nullptr);
}

// -------------------------------------------------------------------------
// Install hooks if no external loader is detected. Call at kPostLoad,
// BEFORE TES4 plugin loading begins.
//
// The list of form types hooked here mirrors Baka Framework's installed
// list. Types that the engine retains EDIDs for natively (keywords,
// global vars, voice types, ...) are intentionally omitted - hooking
// them would be redundant. Types that the engine doesn't retain (NPCs,
// factions, weapons, OMODs, ...) are hooked.
// -------------------------------------------------------------------------
inline void InstallIfNeeded()
{
    if (IsWorking())
    {
        spdlog::info("[EditorIDLoader] EditorIDs already loaded. Skipping install.");
        return;
    }
    std::string external = DetectedLoader();

    if (!external.empty())
    {
        spdlog::info("[EditorIDLoader] External loader detected ({}). Skipping install.", external);
        return;
    }

    spdlog::info("[EditorIDLoader] No external EditorID loader detected. Installing our own hooks "
                 "(adapted from Baka Framework, GPL-3.0).");

    using namespace detail;

    // Mirror of Baka's installed list (uncommented entries from
    // LoadEditorIDs.h). Order doesn't matter; each hook is independent.
    Hook<RE::BGSTransform>::Install();
    Hook<RE::BGSComponent>::Install();
    Hook<RE::BGSTextureSet>::Install();
    Hook<RE::BGSDamageType>::Install();
    Hook<RE::TESClass>::Install();
    Hook<RE::TESFaction>::Install();
    Hook<RE::TESEyes>::Install();
    Hook<RE::BGSAcousticSpace>::Install();
    Hook<RE::EffectSetting>::Install();
    Hook<RE::Script>::Install();
    Hook<RE::TESLandTexture>::Install();
    Hook<RE::EnchantmentItem>::Install();
    Hook<RE::SpellItem>::Install();
    Hook<RE::ScrollItem>::Install();
    Hook<RE::TESObjectACTI>::Install();
    Hook<RE::BGSTalkingActivator>::Install();
    Hook<RE::TESObjectARMO>::Install();
    Hook<RE::TESObjectBOOK>::Install();
    Hook<RE::TESObjectCONT>::Install();
    Hook<RE::TESObjectDOOR>::Install();
    Hook<RE::IngredientItem>::Install();
    Hook<RE::TESObjectLIGH>::Install();
    Hook<RE::TESObjectMISC>::Install();
    Hook<RE::TESObjectSTAT>::Install();
    Hook<RE::TESObjectREFR>::Install();
    Hook<RE::TESObjectCELL>::Install();
    Hook<RE::TESGrass>::Install();
    Hook<RE::TESObjectTREE>::Install();
    Hook<RE::TESFlora>::Install();
    Hook<RE::TESFurniture>::Install();
    Hook<RE::TESObjectWEAP>::Install();
    Hook<RE::TESAmmo>::Install();
    Hook<RE::TESNPC>::Install();
    Hook<RE::TESLevCharacter>::Install();
    Hook<RE::TESLeveledList>::Install();
    Hook<RE::TESKey>::Install();
    Hook<RE::AlchemyItem>::Install();
    Hook<RE::BGSIdleMarker>::Install();
    Hook<RE::BGSNote>::Install();
    Hook<RE::BGSProjectile>::Install();
    Hook<RE::BGSHazard>::Install();
    Hook<RE::BGSBendableSpline>::Install();
    Hook<RE::TESSoulGem>::Install();
    Hook<RE::BGSTerminal>::Install();
    Hook<RE::TESLevItem>::Install();
    Hook<RE::TESWeather>::Install();
    Hook<RE::TESClimate>::Install();
    Hook<RE::BGSShaderParticleGeometryData>::Install();
    Hook<RE::BGSReferenceEffect>::Install();
    Hook<RE::TESRegion>::Install();
    Hook<RE::Explosion>::Install();
    Hook<RE::Projectile>::Install();
    Hook<RE::Actor>::Install();
    Hook<RE::PlayerCharacter>::Install();
    Hook<RE::MissileProjectile>::Install();
    Hook<RE::ArrowProjectile>::Install();
    Hook<RE::GrenadeProjectile>::Install();
    Hook<RE::BeamProjectile>::Install();
    Hook<RE::FlameProjectile>::Install();
    Hook<RE::ConeProjectile>::Install();
    Hook<RE::BarrierProjectile>::Install();
    Hook<RE::Hazard>::Install();
    Hook<RE::TESTopicInfo>::Install();
    Hook<RE::TESPackage>::Install();
    Hook<RE::AlarmPackage>::Install();
    Hook<RE::DialoguePackage>::Install();
    Hook<RE::FleePackage>::Install();
    Hook<RE::SpectatorPackage>::Install();
    Hook<RE::TrespassPackage>::Install();
    Hook<RE::TESCombatStyle>::Install();
    Hook<RE::TESLoadScreen>::Install();
    Hook<RE::TESLevSpell>::Install();
    Hook<RE::TESWaterForm>::Install();
    Hook<RE::TESEffectShader>::Install();
    Hook<RE::BGSExplosion>::Install();
    Hook<RE::BGSDebris>::Install();
    Hook<RE::TESImageSpace>::Install();
    Hook<RE::BGSListForm>::Install();
    Hook<RE::BGSPerk>::Install();
    Hook<RE::BGSBodyPartData>::Install();
    Hook<RE::BGSAddonNode>::Install();
    Hook<RE::BGSCameraShot>::Install();
    Hook<RE::BGSCameraPath>::Install();
    Hook<RE::BGSMaterialType>::Install();
    Hook<RE::BGSImpactData>::Install();
    Hook<RE::BGSImpactDataSet>::Install();
    Hook<RE::TESObjectARMA>::Install();
    Hook<RE::BGSEncounterZone>::Install();
    Hook<RE::BGSLocation>::Install();
    Hook<RE::BGSMessage>::Install();
    Hook<RE::BGSLightingTemplate>::Install();
    Hook<RE::BGSFootstep>::Install();
    Hook<RE::BGSFootstepSet>::Install();
    Hook<RE::BGSDialogueBranch>::Install();
    Hook<RE::BGSMusicTrackFormWrapper>::Install();
    Hook<RE::TESWordOfPower>::Install();
    Hook<RE::TESShout>::Install();
    Hook<RE::BGSEquipSlot>::Install();
    Hook<RE::BGSRelationship>::Install();
    Hook<RE::BGSScene>::Install();
    Hook<RE::BGSAssociationType>::Install();
    Hook<RE::BGSOutfit>::Install();
    Hook<RE::BGSArtObject>::Install();
    Hook<RE::BGSMaterialObject>::Install();
    Hook<RE::BGSMovementType>::Install();
    Hook<RE::BGSDualCastData>::Install();
    Hook<RE::BGSSoundCategory>::Install();
    Hook<RE::BGSSoundOutput>::Install();
    Hook<RE::BGSCollisionLayer>::Install();
    Hook<RE::BGSColorForm>::Install();
    Hook<RE::BGSReverbParameters>::Install();
    Hook<RE::BGSAimModel>::Install();
    Hook<RE::BGSConstructibleObject>::Install();
    Hook<RE::BGSMod::Attachment::Mod>::Install();
    Hook<RE::BGSMaterialSwap>::Install();
    Hook<RE::BGSZoomData>::Install();
    Hook<RE::BGSInstanceNamingRules>::Install();
    Hook<RE::BGSSoundKeywordMapping>::Install();
    Hook<RE::BGSAudioEffectChain>::Install();
    Hook<RE::BGSSoundCategorySnapshot>::Install();
    Hook<RE::BGSSoundTagSet>::Install();
    Hook<RE::BGSLensFlare>::Install();
    Hook<RE::BGSGodRays>::Install();

    spdlog::info("[EditorIDLoader] Hooks installed.");
}

} // namespace BNS::EditorIDLoader
