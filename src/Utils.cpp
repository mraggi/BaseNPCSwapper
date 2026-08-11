#include "Utils.hpp"

#include <format>
#include <spdlog/spdlog.h>

#include <RE/Fallout.h>

namespace BNS::Utils
{

namespace
{
    std::string GetKeywordName(RE::BGSKeyword* a_kw)
    {
        if (!a_kw) return "<None>";
        const char* edid = a_kw->GetFormEditorID();
        return edid ? std::string(edid) : std::format("KW_{:08X}", a_kw->GetFormID());
    }
} // namespace

// EditorID then FullName then "<FormID hex>". Templated NPCs and LVLNs have
// no FullName but do have an EditorID, so the fallback keeps debug logs
// readable instead of forcing a FO4Edit lookup of every FormID.
std::string GetFormName(RE::TESForm* form)
{
    if (!form) return "Unknown";

    // For refs the name lives on the base. The base!=form guard guards
    // against an unlikely self-referential data.objectReference.
    if (auto* ref = form->As<RE::TESObjectREFR>())
    {
        if (auto* base = ref->data.objectReference; base && base != form) { return GetFormName(base); }
    }

    std::string result;
    const char* edid = form->GetFormEditorID();
    if (edid && edid[0] != '\0') { result = edid; }

    if (auto* fullName = form->As<RE::TESFullName>())
    {
        const char* name = fullName->GetFullName();
        if (name && name[0] != '\0')
        {
            if (!result.empty()) result += " | ";
            result += name;
        }
    }

    if (auto* kw = form->As<RE::BGSKeyword>())
    {
        if (!result.empty()) result += " | ";
        result += GetKeywordName(kw);
    }

    if (!result.empty()) return result;
    return std::format("{:X}", form->GetFormID());
}

std::string GetFormName(std::uint32_t id)
{
    RE::TESForm* form = RE::TESForm::GetFormByID(id);
    return GetFormName(form);
}

RE::TESForm* GetTrueBaseForm(RE::Actor* a_actor)
{
    if (!a_actor) return nullptr;
    if (a_actor->extraList)
    {
        if (auto* extraLvl = a_actor->extraList->GetByType<RE::ExtraLeveledCreature>())
        {
            if (extraLvl->originalBase) return extraLvl->originalBase;
        }
    }
    return a_actor->data.objectReference;
}

void SetActorAngle(RE::Actor* a_actor, const RE::NiPoint3& a_rot)
{
    if (!a_actor) return;
    a_actor->data.angle.x = a_rot.x;
    a_actor->data.angle.y = a_rot.y;
    a_actor->data.angle.z = a_rot.z;
    a_actor->Update3DPosition(true);
}

} // namespace BNS::Utils
