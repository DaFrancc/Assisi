/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorInspector.cpp
/// @brief The Inspector panel: reflected component fields, the per-entity
/// replication policy, blueprint-instance placement, and Add Component.
///
/// Undo capture here is record-before-write. The widgets write component memory
/// in place, so a gesture is opened with EditHistory::RecordBefore *before* they
/// run. Structural edits (add/remove a component) commit their own gesture;
/// field edits leave it open and EditorApp's end-of-frame sweep closes it, which
/// is what coalesces a whole drag into one transaction. No field-edit path in
/// this file may close a gesture itself — an early return would then skip the
/// close on exactly the frame it mattered.

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Editor/InspectorFieldChrome.hpp>
#if defined(ASSISI_NETWORKING)
#    include <Assisi/NetSync/NetComponents.hpp>
#endif
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>

#include <imgui.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace Assisi::Editor
{
namespace
{
constexpr ImVec4 kWarnColor{0.9f, 0.8f, 0.3f, 1.f};
constexpr ImVec4 kWireColor{0.45f, 0.8f, 0.95f, 1.f};
/// The same glyph, dimmed: reads as "off" at a glance while staying visible,
/// because its *absence* already means something else — the entity does not
/// replicate at all.
constexpr ImVec4 kWireOffColor{0.42f, 0.42f, 0.46f, 1.f};
/// A plug, from the editor's Nerd Font. Small and wordless, so it does not
/// compete with the component name beside it.
constexpr const char *kWireGlyph = "\xef\x87\xa6"; // U+F1E6
} // namespace

namespace
{

using Assisi::Editor::RadioVisibility;
using Assisi::Editor::ScopedFieldChrome;

const Assisi::Core::Reflect::FieldMeta *FindField(const Assisi::Core::Reflect::ComponentMeta &meta,
                                                  const std::string &name)
{
    for (const auto &candidate : meta.fields)
    {
        if (candidate.name == name)
        {
            return &candidate;
        }
    }
    return nullptr;
}

/// @brief Read a reflected enum from @p fp at its true underlying width.
///
/// The underlying type may be 1, 2, 4 or 8 bytes, and must be read at exactly
/// that width: treating an 8/16-bit enum as a 4-byte int reads neighbouring
/// bytes, and the matching write would clobber them. @p signed_ sign-extends.
std::int64_t ReadEnumValue(const void *fp, std::uint8_t size, bool signed_)
{
    switch (size)
    {
    case 1:
        return signed_ ? std::int64_t(*static_cast<const std::int8_t *>(fp))
                       : std::int64_t(*static_cast<const std::uint8_t *>(fp));
    case 2:
        return signed_ ? std::int64_t(*static_cast<const std::int16_t *>(fp))
                       : std::int64_t(*static_cast<const std::uint16_t *>(fp));
    case 4:
        return signed_ ? std::int64_t(*static_cast<const std::int32_t *>(fp))
                       : std::int64_t(*static_cast<const std::uint32_t *>(fp));
    case 8:
        return *static_cast<const std::int64_t *>(fp);
    default:
        return 0;
    }
}

/// @brief Write @p value into a reflected enum at @p fp at its underlying width.
/// Truncation gives the same bit pattern signed or unsigned, so no sign flag.
void WriteEnumValue(void *fp, std::uint8_t size, std::int64_t value)
{
    switch (size)
    {
    case 1:
        *static_cast<std::int8_t *>(fp) = static_cast<std::int8_t>(value);
        break;
    case 2:
        *static_cast<std::int16_t *>(fp) = static_cast<std::int16_t>(value);
        break;
    case 4:
        *static_cast<std::int32_t *>(fp) = static_cast<std::int32_t>(value);
        break;
    case 8:
        *static_cast<std::int64_t *>(fp) = value;
        break;
    default:
        break;
    }
}

/// @brief Resolve a field's AFIELD(radio) visibility against a live component.
///
/// A listener (radioSource set) follows a sibling broadcaster enum, and that
/// sibling may itself be a listener — so this walks the source chain to the root
/// broadcaster and folds back down. While any source above is inactive the field
/// is Hidden outright; once they are all active, the field is Active if its own
/// source currently holds one of radioValues, and otherwise takes its own
/// radioBehavior. Non-listeners are always Active.
///
/// reflectgen already validates that the chain exists and is acyclic, so the
/// bound on the walk and the lookup misses handled below are defensive only.
RadioVisibility EvaluateRadio(const void *component, const Assisi::Core::Reflect::ComponentMeta &meta,
                              const Assisi::Core::Reflect::FieldMeta &field)
{
    using Assisi::Core::Reflect::FieldMeta;
    using Assisi::Core::Reflect::RadioBehavior;

    if (field.radioSource.empty())
    {
        return RadioVisibility::Active;
    }

    // field, its source, its source's source, ... up to the root broadcaster (a
    // field with no radioSource). Bounded by the field count.
    std::vector<const FieldMeta *> chain;
    for (const FieldMeta *cur = &field; cur != nullptr && chain.size() <= meta.fields.size();)
    {
        chain.push_back(cur);
        if (cur->radioSource.empty())
        {
            break;
        }
        cur = FindField(meta, cur->radioSource);
    }

    // A broadcaster is an enum or a bool, and a bool is read as the 0 or 1 the
    // parser recorded for `false` and `true`. It cannot go through
    // ReadEnumValue: a bool field carries no enumSize, so that would read a
    // zero-width integer rather than the byte.
    const auto readSource = [component](const FieldMeta *fm) -> std::int64_t
                            {
                                const void *fp = static_cast<const char *>(component) + fm->offset;
                                if (fm->type == Assisi::Core::Reflect::FieldType::Bool)
                                {
                                    return *static_cast<const bool *>(fp) ? 1 : 0;
                                }
                                return ReadEnumValue(fp, fm->enumSize, fm->enumSigned);
                            };

    // Fold from the root down toward `field` (chain front). `state` holds the
    // resolved visibility of the source one level up.
    RadioVisibility state = RadioVisibility::Active; // the root broadcaster
    for (std::size_t i = chain.size(); i-- > 1;)
    {
        const FieldMeta *listener = chain[i - 1];
        const FieldMeta *source   = chain[i];
        if (state != RadioVisibility::Active)
        {
            state = RadioVisibility::Hidden; // an inactive source hides its listeners
            continue;
        }
        const std::int64_t current = readSource(source);
        bool match   = false;
        for (const std::int64_t value : listener->radioValues)
        {
            if (current == value)
            {
                match = true;
                break;
            }
        }
        state = match ? RadioVisibility::Active
                      : (listener->radioBehavior == RadioBehavior::Vanish ? RadioVisibility::Hidden
                                                                          : RadioVisibility::Greyed);
    }
    return state;
}

} // namespace

bool EditorApp::EditFieldValue(void *fp, const Assisi::Core::Reflect::FieldMeta &field,
                               const Assisi::Core::Reflect::FieldBounds &bounds)
{
    using namespace Assisi::Core::Reflect;

    bool edited = false;
    switch (field.type)
    {
    case FieldType::Float:
    {
        // AFIELD(min=/max=) hints, already resolved — a bound naming a sibling
        // field is that sibling's current value. DragFloat reads min==max==0 as
        // "no clamp", so an open side substitutes ±FLT_MAX; AlwaysClamp is what
        // makes the bounds hold for Ctrl+click text entry too.
        const float minBound = bounds.hasMin ? static_cast<float>(bounds.minValue) : -FLT_MAX;
        const float maxBound = bounds.hasMax ? static_cast<float>(bounds.maxValue) : FLT_MAX;
        edited = ImGui::DragFloat(field.name.c_str(), static_cast<float *>(fp), 0.01f, minBound, maxBound,
                                  "%.3f", ImGuiSliderFlags_AlwaysClamp);
        break;
    }
    case FieldType::Double:
    {
        edited = ImGui::InputDouble(field.name.c_str(), static_cast<double *>(fp));
        // InputDouble cannot clamp, so the bounds are applied after the edit.
        if (edited)
        {
            double &value = *static_cast<double *>(fp);
            if (bounds.hasMin)
            {
                value = std::max(value, bounds.minValue);
            }
            if (bounds.hasMax)
            {
                value = std::min(value, bounds.maxValue);
            }
        }
        break;
    }
    case FieldType::Int32:
    {
        // reflectgen guarantees an integer field's bounds are integral and in
        // range, so these casts are exact.
        const int32_t minBound = bounds.hasMin ? static_cast<int32_t>(bounds.minValue) : INT32_MIN;
        const int32_t maxBound = bounds.hasMax ? static_cast<int32_t>(bounds.maxValue) : INT32_MAX;
        edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_S32, fp, 1.f, &minBound, &maxBound,
                                   nullptr, ImGuiSliderFlags_AlwaysClamp);
        break;
    }
    case FieldType::UInt32:
    {
        const uint32_t minBound = bounds.hasMin ? static_cast<uint32_t>(bounds.minValue) : 0u;
        const uint32_t maxBound = bounds.hasMax ? static_cast<uint32_t>(bounds.maxValue) : UINT32_MAX;
        edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_U32, fp, 1.f, &minBound, &maxBound,
                                   nullptr, ImGuiSliderFlags_AlwaysClamp);
        break;
    }
    case FieldType::Int64:
    {
        // Bounds arrive as double, exact only to 2^53. Past that a bound would
        // silently round, so the open range stops at what is representable
        // rather than pretending to honour it.
        constexpr int64_t kExact   = 1LL << 53;
        const int64_t minBound = bounds.hasMin ? static_cast<int64_t>(bounds.minValue) : -kExact;
        const int64_t maxBound = bounds.hasMax ? static_cast<int64_t>(bounds.maxValue) : kExact;
        edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_S64, fp, 1.f, &minBound, &maxBound,
                                   nullptr, ImGuiSliderFlags_AlwaysClamp);
        break;
    }
    case FieldType::UInt64:
    {
        constexpr uint64_t kExact   = 1ULL << 53;
        const uint64_t minBound = bounds.hasMin ? static_cast<uint64_t>(bounds.minValue) : 0u;
        const uint64_t maxBound = bounds.hasMax ? static_cast<uint64_t>(bounds.maxValue) : kExact;
        edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_U64, fp, 1.f, &minBound, &maxBound,
                                   nullptr, ImGuiSliderFlags_AlwaysClamp);
        break;
    }
    case FieldType::Bool:
        edited = ImGui::Checkbox(field.name.c_str(), static_cast<bool *>(fp));
        break;
    case FieldType::Enum:
    {
        // Stored as the underlying integer, whose width varies per AENUM —
        // hence the read/write at field.enumSize rather than a plain int.
        const std::int64_t value = ReadEnumValue(fp, field.enumSize, field.enumSigned);
        const char *preview = "(unknown)";
        for (const auto &constant : field.enumConstants)
        {
            if (constant.value == value)
            {
                preview = constant.name.c_str();
                break;
            }
        }
        if (ImGui::BeginCombo(field.name.c_str(), preview))
        {
            for (const auto &constant : field.enumConstants)
            {
                const bool selected = (constant.value == value);
                if (ImGui::Selectable(constant.name.c_str(), selected))
                {
                    WriteEnumValue(fp, field.enumSize, constant.value);
                    edited = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case FieldType::String:
    {
        // Core::ShortString is the only String type today.
        auto *str = static_cast<Assisi::Core::ShortString *>(fp);
        char buf[Assisi::Core::kShortStringMax + 1];
        str->ToCStr(buf, sizeof(buf));
        if (ImGui::InputText(field.name.c_str(), buf, sizeof(buf)))
        {
            str->Assign(buf);
            edited = true;
        }
        break;
    }
    case FieldType::EntityName:
    {
        // The String box over the wider buffer. Not the entity's own Name —
        // that one has to stay unique, so the rename box above owns it.
        auto *name = static_cast<Assisi::Core::EntityName *>(fp);
        char buf[Assisi::Core::kEntityNameMax + 1];
        name->ToCStr(buf, sizeof(buf));
        if (ImGui::InputText(field.name.c_str(), buf, sizeof(buf)))
        {
            name->Assign(buf);
            edited = true;
        }
        break;
    }
    case FieldType::Vec2:
        edited = ImGui::DragFloat2(field.name.c_str(), static_cast<float *>(fp), 0.01f);
        break;
    case FieldType::Vec3:
        edited = ImGui::DragFloat3(field.name.c_str(), static_cast<float *>(fp), 0.01f);
        break;
    case FieldType::Vec4:
        edited = ImGui::DragFloat4(field.name.c_str(), static_cast<float *>(fp), 0.01f);
        break;
    // Colours are linear and may exceed 1 (an emissive factor is a radiance
    // multiplier, not a display colour), so the picker runs in float/HDR mode
    // — the default 8-bit mode would quantize the value and clamp the range.
    case FieldType::Color3:
        edited = ImGui::ColorEdit3(field.name.c_str(), static_cast<float *>(fp),
                                   ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        break;
    case FieldType::Color4:
        edited = ImGui::ColorEdit4(field.name.c_str(), static_cast<float *>(fp),
                                   ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                   ImGuiColorEditFlags_AlphaPreviewHalf);
        break;
    case FieldType::Quat:
    {
        auto *quat  = static_cast<glm::quat *>(fp);
        glm::vec3 euler = glm::degrees(glm::eulerAngles(*quat));
        if (ImGui::DragFloat3(field.name.c_str(), &euler.x, 0.5f))
        {
            *quat  = glm::normalize(glm::quat(glm::radians(euler)));
            edited = true;
        }
        break;
    }
    default:
        // Either a type only an owning component can draw (the caller handles
        // those before delegating here) or one nothing draws yet.
        ImGui::TextDisabled("%s: [unsupported type]", field.name.c_str());
        break;
    }
    return edited;
}

bool EditorApp::EditComponentFields(void *mut, const Assisi::Core::Reflect::ComponentMeta &meta)
{
    using namespace Assisi::Core::Reflect;

    bool anyEditable   = false;
    bool anyFieldEdited = false;

    for (const auto &field : meta.fields)
    {
        if (field.transient)
            continue;
        anyEditable = true;

        // Hide or grey an AFIELD(radio) listener whose sibling enum is not at one
        // of its active values, then scope the AFIELD(norep) dimming to what is
        // actually drawn — the verdict first, so the `continue` below has no
        // colour push to unwind.
        const RadioVisibility radio = EvaluateRadio(mut, meta, field);
        const bool serverOnly       = field.norep;
        ScopedFieldChrome chrome{radio, serverOnly};
        if (!chrome.Visible())
        {
            continue;
        }
        const bool greyed = chrome.Greyed();

        void *fp = static_cast<char *>(mut) + field.offset;
        ImGui::PushID(field.name.c_str());
        if (greyed)
        {
            ImGui::BeginDisabled();
        }

        // The cases here are the ones that need the *owning component* — a
        // browse target pinned as (entity, meta, offset), a scene to list
        // entities from, or a sibling field. Everything decided by the field's
        // type alone falls through to EditFieldValue, which the material panel
        // shares.
        bool edited = false;
        switch (field.type)
        {
        case FieldType::AssetPath:
        {
            auto *ap = static_cast<Assisi::Core::AssetPath *>(fp);
            char buf[Assisi::Core::kAssetPathMax + 1];
            ap->ToCStr(buf, sizeof(buf));

            // Laid out as [ input ][…] fieldName. The input's own label is
            // suppressed (the "##" id) so the button can sit between it and the
            // visible label, and the input is narrowed to leave room for both.
            const float browseW = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - browseW - ImGui::GetStyle().ItemSpacing.x);
            const std::string inputId = "##" + field.name;
            if (ImGui::InputText(inputId.c_str(), buf, sizeof(buf)))
            {
                ap->Assign(buf);
                edited = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("...", ImVec2(browseW, 0.f)))
                OpenAssetBrowserFor(meta, field.offset);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse assets");
            ImGui::SameLine();
            ImGui::TextUnformatted(field.name.c_str());
            break;
        }
        case FieldType::AssetId:
        {
            // Stored as a GUID, shown and edited as its resolved virtual path —
            // the database translates both ways. Same [ input ][…] label layout
            // as AssetPath above.
            auto *id      = static_cast<Assisi::Core::AssetId *>(fp);
            const std::string inputId = "##" + field.name;
            if (AssetIdPathField(inputId.c_str(), *id))
                edited = true;
            ImGui::SameLine();
            if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0.f)))
                OpenAssetBrowserFor(meta, field.offset);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Browse assets");
            ImGui::SameLine();
            ImGui::TextUnformatted(field.name.c_str());
            break;
        }
        case FieldType::EntityRef:
        {
            auto *ref   = static_cast<Assisi::ECS::Entity *>(fp);
            const bool empty = (*ref == Assisi::ECS::NullEntity);

            std::string preview;
            if (empty)
                preview = "(none)";
            else if (!_scene->IsAlive(*ref))
                preview = std::format("[{}:{}] (dangling)", ref->index, ref->generation);
            else
                preview = DescribeEntity(*ref);

            const bool armedForThis = _eyedropperArmed && _eyedropperMeta == &meta &&
                                      _eyedropperFieldOffset == field.offset;
            const char *pickLabel    = armedForThis ? "Cancel" : "Pick";

            /* Eyedropper first, then the combo: drawn the other way round, the
               combo's trailing label pushes the button off the window's right
               edge. */
            if (ImGui::Button(pickLabel))
            {
                if (armedForThis)
                {
                    _eyedropperArmed = false;
                    _eyedropperMeta  = nullptr;
                }
                else
                {
                    _eyedropperArmed       = true;
                    _eyedropperEntity      = _selectedEntity;
                    _eyedropperMeta        = &meta;
                    _eyedropperFieldOffset = field.offset;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(armedForThis ? "Picking… left-click an entity in the scene (or click to cancel)"
                                               : "Eyedropper: then left-click an entity in the scene");

            ImGui::SameLine();
            if (ImGui::BeginCombo(field.name.c_str(), preview.c_str()))
            {
                if (ImGui::Selectable("(none)", empty))
                {
                    *ref   = Assisi::ECS::NullEntity;
                    edited = true;
                }
                _scene->ForEachEntity(
                    [&](Assisi::ECS::Entity e)
                        {
                            const std::string label    = DescribeEntity(e);
                            const bool selected = (e == *ref);
                            if (ImGui::Selectable(label.c_str(), selected))
                            {
                                *ref   = e;
                                edited = true;
                            }
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                        });
                ImGui::EndCombo();
            }
            break;
        }
        case FieldType::AssetIdVector:
        {
            // MeshRenderer::materialOverrides is the only field of this type; it
            // gets one browse row per material slot of the resolved mesh.
            if (meta.name == "MeshRenderer" && field.name == "materialOverrides")
                edited = EditMaterialSlots(*static_cast<Assisi::Runtime::MeshRenderer *>(mut), meta, field.offset);
            else
                ImGui::TextDisabled("%s: [unsupported vector]", field.name.c_str());
            break;
        }
        case FieldType::InstanceRef:
        {
            auto *ref = static_cast<Assisi::ECS::InstanceId *>(fp);

            // Named by source and id rather than by id alone: the number is a
            // per-world counter and means nothing to the author on its own.
            const auto describe = [this](Assisi::ECS::InstanceId id) -> std::string
                                  {
                                      if (!id.IsValid())
                                          return "(none)";
                                      if (_world == nullptr)
                                          return std::format("instance {}", id.value);
                                      const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(id);
                                      if (row == nullptr)
                                          return std::format("instance {} (missing)", id.value);
                                      return std::format("{} ({})", row->name.empty() ? row->source : row->name, id.value);
                                  };

            if (ImGui::BeginCombo(field.name.c_str(), describe(*ref).c_str()))
            {
                if (ImGui::Selectable("(none)", !ref->IsValid()))
                {
                    *ref   = Assisi::ECS::NullInstance;
                    edited = true;
                }
                if (_world != nullptr)
                {
                    for (const auto &[id, row] : _world->instances.All())
                    {
                        const bool selected = (id == *ref);
                        if (ImGui::Selectable(describe(id).c_str(), selected))
                        {
                            *ref   = id;
                            edited = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        default:
            // Resolved here, against the component being drawn: a bound naming a
            // sibling is only a number once there is an object to read it from.
            edited = EditFieldValue(fp, field, ResolveFieldBounds(field, meta.fields, mut));
            break;
        }

        if (greyed)
        {
            ImGui::EndDisabled();
        }
        if (serverOnly)
        {
            // Dropped here rather than at end of scope so the tag and its tooltip
            // draw in their own colour.
            chrome.EndTint();
            ImGui::SameLine();
            ImGui::TextDisabled("(server-only)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("AFIELD(norep): saved with the level, never sent to clients. Every client "
                                  "holds this field's default.");
            }
        }
        // A marker beside each field this instance claims, right-clickable to drop
        // the claim. Per field, not per component, because that is the granularity
        // the override record has: resetting one field leaves the rest following
        // the blueprint.
        if (const nlohmann::json *claim = OverrideClaimFor(_selectedEntity, meta.name);
            claim != nullptr && claim->is_object() && claim->contains(field.name))
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.f, 0.82f, 0.4f, 1.f});
            ImGui::TextUnformatted("*");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Overridden by this instance. Right-click to reset it.");

            if (ImGui::BeginPopupContextItem("##resetfield"))
            {
                // Queued, never applied here: the reset removes and re-adds the
                // component, and this loop still holds a pointer into the pool
                // that would move underneath it. Drained after the loop.
                if (ImGui::MenuItem("Reset to blueprint"))
                    _pendingOverrideReset = PendingOverrideReset{_selectedEntity, meta.name, field.name};
                ImGui::EndPopup();
            }
        }

        anyFieldEdited |= edited;
        ImGui::PopID();
    }

    if (!anyEditable)
        ImGui::TextDisabled("(runtime-only)");

    return anyFieldEdited;
}

bool EditorApp::EditMaterialSlots(Assisi::Runtime::MeshRenderer &mrc,
                                  const Assisi::Core::Reflect::ComponentMeta &meta, std::size_t fieldOffset)
{
    ImGui::TextUnformatted("materialOverrides");

    if (mrc.meshBuffer == nullptr)
    {
        ImGui::TextDisabled("  (resolve a mesh to edit its materials)");
        return false;
    }

    const std::vector<Assisi::Geometry::MaterialData> &slots = mrc.meshBuffer->Materials();
    if (slots.empty())
    {
        ImGui::TextDisabled("  (mesh has no material slots)");
        return false;
    }

    bool edited = false;
    for (std::size_t slot = 0; slot < slots.size(); ++slot)
    {
        ImGui::PushID(static_cast<int32_t>(slot));

        // A nil id means the slot falls back to the mesh's imported default. The
        // overrides vector may be shorter than the slot list.
        Assisi::Core::AssetId slotId;
        if (slot < mrc.materialOverrides.size())
            slotId = mrc.materialOverrides[slot];

        if (AssetIdPathField("##override", slotId))
        {
            if (mrc.materialOverrides.size() <= slot)
                mrc.materialOverrides.resize(slot + 1);
            mrc.materialOverrides[slot] = slotId;
            edited = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0.f)))
            OpenAssetBrowserForSlot(meta, fieldOffset, static_cast<int32_t>(slot));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Browse materials (.amat)");
        ImGui::SameLine();

        // Edit what this slot actually draws: its override, or — when there is
        // none — the material the mesh imported for the slot, which is the thing
        // on screen. Both are ordinary `.amat` files, and the panel shows the
        // path, so editing a shared mesh default is visible rather than implied.
        const Assisi::Core::AssetId effectiveId =
            slotId.IsNil() ? _assetDatabase.SlotMaterial(mrc.mesh, static_cast<std::uint32_t>(slot)) : slotId;
        const std::optional<std::string> effectivePath = _assetDatabase.PathFor(effectiveId);
        ImGui::BeginDisabled(!effectivePath.has_value());
        if (ImGui::Button("Edit"))
        {
            OpenMaterialEditorForSlot(*effectivePath, _selectedEntity, fieldOffset, static_cast<int32_t>(slot));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            if (!effectivePath.has_value())
                ImGui::SetTooltip("This slot has no material file to edit.");
            else if (slotId.IsNil())
                ImGui::SetTooltip("Edit '%s' — the mesh's own material for this slot, shared by everything "
                                  "that uses this mesh.",
                                  effectivePath->c_str());
            else
                ImGui::SetTooltip("Edit '%s' in the Material panel.", effectivePath->c_str());
        }
        ImGui::SameLine();

        // Labelled by the mesh's imported material name, index if it has none.
        const std::string &name = slots[slot].Name;
        if (name.empty())
            ImGui::Text("slot %zu", slot);
        else
            ImGui::Text("slot %zu — %s", slot, name.c_str());

        ImGui::PopID();
    }

    return edited;
}

bool EditorApp::AssetIdPathField(const char *inputId, Assisi::Core::AssetId &id)
{
    // Shows the id as its current virtual path; typing a path re-resolves the id
    // through the database, to nil if the path is unknown. Only the input is drawn
    // — the caller owns the browse button and label to its right, so the width is
    // reserved for them here.
    char buf[Assisi::Core::kAssetPathMax + 1] = {};
    if (const std::optional<std::string> path = _assetDatabase.PathFor(id))
        std::snprintf(buf, sizeof(buf), "%s", path->c_str());

    const float browseW = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - browseW - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText(inputId, buf, sizeof(buf)))
    {
        id = _assetDatabase.IdFor(buf).value_or(Assisi::Core::AssetId{});
        return true;
    }
    return false;
}

void EditorApp::HandlePhysicsEditing(bool anyFieldEdited)
{
    if (anyFieldEdited)
    {
        const auto *tc   = _scene->Get<Assisi::Runtime::Transform>(_selectedEntity);
        const auto *rbc  = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity);
        const auto *desc = _scene->Get<Assisi::Physics::RigidBodyDescriptor>(_selectedEntity);
        if (tc && rbc)
            _physics->SetBodyTransform(*rbc, tc->position, tc->rotation);
        if (rbc && desc)
        {
            _physics->ReshapeBody(*rbc, Assisi::Physics::PhysicsWorld::ColliderShapeDesc{
                    .shape       = desc->shape,
                    .halfExtents = desc->halfExtents,
                    .radius      = desc->radius,
                    .halfHeight  = desc->halfHeight});
            _physics->SetBodyCCD(*rbc, desc->enableCCD);
        }
    }

    // IsAnyItemActive() is global: it fires for a drag in *any* window, so the AA
    // combo or the Save-As field would otherwise freeze the selected body to
    // Static. The IsWindowFocused test scopes it to the Inspector — this runs
    // inside the Inspector's Begin/End, so "current window" means this panel.
    const bool nowDragging =
        ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (nowDragging)
        RequestPhysicsFreeze(); // the release is handled at end of frame, not here
}

void EditorApp::RequestPhysicsFreeze()
{
    // Raised every frame the gesture is held; OnImGui thaws on the first frame it
    // is *not* raised. That is what makes the release survive this function not
    // being called at all — nothing selected, entity destroyed mid-drag, the world
    // selector moved to an inspect-only world.
    _physicsFreezeRequested = true;

    if (_frozenBodyEntity == _selectedEntity)
        return; // already held

    // Selection moved while a gesture was live. Release the previous body before
    // taking this one, or it stays Static with a descriptor that still says
    // dynamic — a body stuck in mid-air that nothing in the UI explains.
    if (_frozenBodyEntity != Assisi::ECS::NullEntity)
        ThawEditedBody();

    // Everything below goes through _world, not the _scene/_physics shortcuts, so
    // the hold is taken in exactly the world ThawEditedBody will look up by name.
    // They track the same world today; not relying on that is cheap insurance.
    if (_world == nullptr || _selectedEntity == Assisi::ECS::NullEntity ||
        !_world->scene.IsAlive(_selectedEntity))
        return;

    const auto *rbc = _world->scene.Get<Assisi::Physics::RigidBody>(_selectedEntity);
    if (rbc == nullptr)
        return; // nothing to hold; a body added mid-gesture is picked up next frame

    _world->physics.SetBodyMotionType(*rbc, Assisi::Physics::BodyMotion::Static);
    _frozenBodyEntity = _selectedEntity;
    _frozenBodyWorld  = _world->name;
}

void EditorApp::ThawEditedBody()
{
    if (_frozenBodyEntity == Assisi::ECS::NullEntity)
        return;

    const Assisi::ECS::Entity entity = _frozenBodyEntity;
    const std::string worldName = _frozenBodyWorld;

    // Cleared up front so the bail-out paths below (world gone, entity destroyed)
    // cannot leave a record pointing at something that no longer exists.
    _frozenBodyEntity = Assisi::ECS::NullEntity;
    _frozenBodyWorld.clear();

    // By the recorded name, not the app's current _scene/_physics: the viewed world
    // can have changed since the freeze, and thawing against the wrong one leaves
    // the real body frozen while poking an unrelated body.
    Assisi::App::World *world = _worlds.Find(worldName);
    if (world == nullptr || !world->scene.IsAlive(entity))
        return; // world or entity is gone; its body went with it

    const auto *rbc = world->scene.Get<Assisi::Physics::RigidBody>(entity);
    if (rbc == nullptr)
        return; // collider removed during the drag

    // Back to whatever the descriptor authored, never unconditionally Dynamic: the
    // freeze must be invisible, including for bodies that were Static all along.
    const auto *desc     = world->scene.Get<Assisi::Physics::RigidBodyDescriptor>(entity);
    const bool isStatic = desc && desc->isStatic;
    world->physics.SetBodyMotionType(*rbc, isStatic ? Assisi::Physics::BodyMotion::Static
                                                    : Assisi::Physics::BodyMotion::Dynamic);
    if (!isStatic)
    {
        // Land it on wherever the drag left the Transform, so it resumes from the
        // pose the author sees rather than the pre-drag one.
        if (const auto *tc = world->scene.Get<Assisi::Runtime::Transform>(entity))
            world->physics.SetBodyTransform(*rbc, tc->position, tc->rotation);
    }
}

void EditorApp::AddComponentToSelected(const Assisi::Core::Reflect::ComponentMeta &meta)
{
    if (_selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity))
    {
        return;
    }
    // Adding one the entity already has would overwrite it. (The Add Component
    // field filters these out of its suggestions too; this is the backstop.)
    if (meta.getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation) != nullptr)
    {
        return;
    }

    // One transaction: record the absent-before state here, commit only after the
    // add *and* its side effects below (camera-facing placement, physics body), so
    // undo restores exactly what the author saw.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, meta.id, EditLabel("Add " + meta.name, _selectedEntity),
                              _selectedEntity);

    // Default-construct through the level loader's own path: its per-field
    // if-contains deserialization leaves every field at its default when given an
    // empty object, so no component needs a bespoke "make default" hook. Nothing
    // in an empty object can be rejected, so the failure branch is unreachable
    // today; it is kept because that is a property of the argument, not the call.
    if (!meta.addToScene(_scene, _selectedEntity.index, _selectedEntity.generation,
                         nlohmann::json::object()))
    {
        Assisi::Core::Log::Error("Editor: could not add a default '{}'.", meta.name);
        return;
    }

    // A few components carry runtime state beyond their reflected fields. Wire it
    // up here so the add takes effect now rather than at the next level reload.
    if (meta.name == "Transform")
    {
        // Entities start transform-less, so place the new one in front of the
        // camera rather than at the world origin. GetMut, not Get: Transform is
        // ACOMP(tracked) and the Add above stamped only the default pose — this
        // write replaces it, and consumers must see a tick for the value they will
        // actually read.
        if (auto *tc = _scene->GetMut<Assisi::Runtime::Transform>(_selectedEntity))
        {
            RefreshCameraMatrix();
            const glm::vec3 forward =
                glm::normalize(_cameraTransform.rotation * glm::vec3(0.f, 0.f, -1.f));
            constexpr float kSpawnDistance = 5.f;
            tc->position = _cameraTransform.position + forward * kSpawnDistance;
        }
    }
    else if (meta.name == "MeshRenderer")
    {
        ReresolveEntityAssets(_selectedEntity); // nil mesh → fallback cube, so it draws
    }
    else if (meta.name == "RigidBodyDescriptor")
    {
        const auto *tc   = _scene->Get<Assisi::Runtime::Transform>(_selectedEntity);
        const auto *desc = _scene->Get<Assisi::Physics::RigidBodyDescriptor>(_selectedEntity);
        if (tc != nullptr && desc != nullptr &&
            _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity) == nullptr)
        {
            _physics->AddBodyFromDescriptor(*_scene, _selectedEntity, *tc, *desc,
                                            Assisi::App::ParentWorldResolver(*_scene));
        }
    }

    if (history != nullptr)
        history->CommitGesture(_selectedEntity, meta.id);
}

void EditorApp::RemoveComponentFromSelected(const Assisi::Core::Reflect::ComponentMeta &meta)
{
    if (_selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity))
    {
        return;
    }

    // Capture the present-before state so undo can bring the component back.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, meta.id, EditLabel("Remove " + meta.name, _selectedEntity),
                              _selectedEntity);

    // Tear down runtime state living outside the reflected fields, before the pool
    // entry disappears. A RigidBodyDescriptor owns a Jolt body through the
    // transient RigidBody handle, so both go.
    //
    // Transform is in this branch too: a body's pose is driven from it, so
    // removing the Transform while the descriptor stays would leave a live Jolt
    // body simulating with nothing to sync it — an orphan only a level reload
    // clears. The descriptor itself survives either way.
    //
    // MeshRenderer needs nothing: its transient pointers are non-owning, the
    // AssetCache owns the GPU resources.
    if (meta.name == "RigidBodyDescriptor" || meta.name == "Transform")
    {
        if (const auto *rbc = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity))
        {
            _physics->RemoveBody(*rbc);
        }
        _scene->Remove<Assisi::Physics::RigidBody>(_selectedEntity);
    }

    _scene->RemoveById(_selectedEntity, meta.id);

    if (history != nullptr)
        history->CommitGesture(_selectedEntity, meta.id);
}

// The inspector's whole replication surface. Declarations in EditorApp.hpp.
#if defined(ASSISI_NETWORKING)
void EditorApp::DrawReplicationSection(bool mirrored)
{
    using namespace Assisi::Core::Reflect;

    // The marker's presence is the only gate, and there is deliberately no
    // checkbox here duplicating it: the marker is an ordinary component, added and
    // removed through the same Add Component menu and the same undo path as
    // anything else. A second control meaning the same thing would be a second
    // place for the two to disagree.
    const bool isReplicated = _scene->Has<Assisi::NetSync::Replicated>(_selectedEntity);

    // The session-scoped identity, from whichever side is looking. It is the only
    // name the two machines share — an entity handle means nothing across a
    // connection — so "which one is this on the other screen" has no other answer.
    if (_netSession != nullptr && _netSession->IsActive())
    {
        Assisi::NetSync::NetId netId = Assisi::NetSync::InvalidNetId;
        if (const Assisi::NetSync::ReplicationServer *server = _netSession->Server())
            netId = server->NetIdOf(_selectedEntity);
        else if (const Assisi::NetSync::ReplicationClient *client = _netSession->Client())
            netId = client->NetIdOf(_selectedEntity);

        if (netId != Assisi::NetSync::InvalidNetId)
            ImGui::TextDisabled("NetId %u", netId.value);
    }

    if (mirrored)
    {
        ImGui::TextColored(ImVec4{0.55f, 0.75f, 1.f, 1.f}, "Mirrored — the host owns this entity.");

        // Which of the two client timelines this entity is on. The discriminator
        // is a replicated RigidBodyDescriptor, invisible in the world, so "why do
        // these two lag differently" has no visible answer without this line.
        const bool bodied = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity) != nullptr;
        ImGui::TextDisabled("Replication path: %s", bodied ? "body-corrected" : "interpolated");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(bodied ? "Simulated locally and re-anchored by the host's corrections. Renders at "
                              "host time minus transit."
                                     : "No local simulation, so it is interpolated between received snapshots — "
                              "about two snapshot intervals behind.");
        }

        // Derived from the components that arrived, **never** from this mirror's
        // local Replicated marker. The client default-constructs that marker when
        // it creates the entity — the host's real one was stripped from the joined
        // world — so rendering its exclusion mask would show an always-empty
        // policy dressed up as the host's. Observed presence is the only thing a
        // client truthfully knows.
        if (ImGui::TreeNode("Receiving"))
        {
            for (const ComponentMeta *meta : ComponentRegistry::Instance().ReplicableComponents())
            {
                const bool present =
                    meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation) != nullptr;
                if (present)
                    ImGui::BulletText("%s", meta->name.c_str());
                else
                    ImGui::TextDisabled("    %s — not sent", meta->name.c_str());
            }
            ImGui::TextDisabled("Policy is the host's; a joined client cannot author it.");
            ImGui::TreePop();
        }

        // Who controls it, if anyone. Read from the replicated component, so it is
        // an observed fact — unlike the local Replicated marker above.
        if (const auto *claim = _scene->Get<Assisi::NetSync::ControlledBy>(_selectedEntity))
        {
            const bool mine = _netSession != nullptr && _netSession->ControlsEntity(_selectedEntity);
            ImGui::TextDisabled("Controlled by client %u%s", claim->client, mine ? " (you)" : "");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Assigned by the host at runtime and replicated like any other component. "
                                  "Never authored — a client id belongs to one session.");
            }
        }
    }

    if (!isReplicated)
    {
        ImGui::Separator();
        return;
    }

    // --- what will actually happen to this entity --------------------------
    // Each warning below marks a mismatch between what the author marked and what
    // the wire will do with it. All of them look fine locally.

    // Nothing to send. Transform is replicated and almost everything has one, so
    // this only fires on genuinely empty entities.
    bool anyReplicatedComponent = false;
    for (const ComponentMeta *meta : ComponentRegistry::Instance().SerializableComponents())
    {
        if (meta->replicable && meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation))
        {
            anyReplicatedComponent = true;
            break;
        }
    }
    if (!anyReplicatedComponent)
    {
        ImGui::TextColored(kWarnColor,
                           "Marked Replicated, but none of its components are — clients get an empty entity.");
    }

    // Hierarchy does not replicate: a marked entity in one loses it, on both ends.
    // The EntityRef machinery works; what is unsolved is the semantics — mirrored
    // children of local parents, transform spaces, world-space body state under a
    // parent-relative Transform.
    if (_scene->Get<Assisi::Runtime::Parent>(_selectedEntity) != nullptr)
    {
        ImGui::TextColored(kWarnColor, "Parented — mirrors are flat in v1, so clients see this at world space.");
    }

    bool hasChildren = false;
    _scene->ForEachEntity(
        [&](Assisi::ECS::Entity candidate)
        {
            if (hasChildren)
                return;
            const auto *parent = _scene->Get<Assisi::Runtime::Parent>(candidate);
            hasChildren        = parent != nullptr && parent->parent == _selectedEntity;
        });
    if (hasChildren)
    {
        ImGui::TextColored(kWarnColor,
                           "Has children — a joining client strips this entity from its copy of the level, and "
                           "its children lose their parent link.");
    }

    // Excluding Transform is legal — a pure-data entity has no spatial meaning to
    // send — but on a *placed* entity it produces a mirror stuck at whatever pose
    // the level file had. The server honours the authored policy either way; this
    // warns instead of quietly overriding what someone wrote.
    if (const auto *marker = _scene->Get<Assisi::NetSync::Replicated>(_selectedEntity))
    {
        const std::size_t transformOrdinal =
            ComponentRegistry::Instance().ReplicableOrdinalOf(ComponentIdOf<Assisi::ECS::Transform>());
        const bool placementDependent =
            _scene->Get<Assisi::Runtime::MeshRenderer>(_selectedEntity) != nullptr ||
            _scene->Get<Assisi::Physics::RigidBodyDescriptor>(_selectedEntity) != nullptr;

        if (marker->excluded.Test(transformOrdinal) && placementDependent)
        {
            ImGui::TextColored(kWarnColor,
                               "Transform is unticked, but this entity is placed — mirrors will sit at the "
                               "level file's pose and never move.");
        }
    }

    // --- per-component policy ----------------------------------------------
    // The capability/policy split, made authorable: every capable component this
    // entity carries gets a checkbox, and unticking one authors an exclusion. An
    // empty mask means everything ticked, which is the default.
    if (!mirrored)
        DrawReplicationPolicy();

    ImGui::Separator();
}

bool EditorApp::IsComponentGameVetoed(const Assisi::Core::Reflect::ComponentMeta &meta) const
{
    return std::find(_netVetoedComponentNames.begin(), _netVetoedComponentNames.end(), meta.name) !=
           _netVetoedComponentNames.end();
}

bool EditorApp::SelectedEntitySends(const Assisi::Core::Reflect::ComponentMeta &meta) const
{
    if (!meta.replicable || IsComponentGameVetoed(meta))
        return false;

    const auto *marker = _scene->Get<Assisi::NetSync::Replicated>(_selectedEntity);
    if (marker == nullptr)
        return false; // the entity does not replicate at all

    const std::size_t ordinal = Assisi::Core::Reflect::ComponentRegistry::Instance().ReplicableOrdinalOf(meta.id);
    return !marker->excluded.Test(ordinal);
}

void EditorApp::SetSelectedEntitySends(const Assisi::Core::Reflect::ComponentMeta &meta, bool sends)
{
    const std::size_t ordinal = Assisi::Core::Reflect::ComponentRegistry::Instance().ReplicableOrdinalOf(meta.id);
    if (ordinal == Assisi::Core::Reflect::ComponentRegistry::kInvalidOrdinal)
        return;

    // A policy change is an edit like any other, so it opens a gesture on the
    // Replicated marker; the end-of-frame sweep commits it.
    if (Assisi::Editor::EditHistory *history = ActiveHistory())
    {
        history->RecordBefore(_selectedEntity,
                              Assisi::Core::Reflect::ComponentIdOf<Assisi::NetSync::Replicated>(),
                              EditLabel("Replication policy", _selectedEntity), _selectedEntity);
    }

    if (Assisi::NetSync::Replicated *marker = _scene->GetMut<Assisi::NetSync::Replicated>(_selectedEntity))
        marker->excluded.Set(ordinal, !sends);
}

void EditorApp::DrawRelevancePolicy()
{
    const auto *marker = _scene->Get<Assisi::NetSync::Replicated>(_selectedEntity);
    if (marker == nullptr)
        return;

    // A mirror's Replicated marker is default-constructed by the client; the
    // host's never travels. Showing its relevance as an authorable dropdown would
    // pass fabricated data off as the host's policy — the same mistake the
    // Receiving list above exists to avoid. Mirrors show observed facts only.
    if (IsMirrored(_selectedEntity))
        return;

    static constexpr const char *kLabels[] = {"Default (the provider decides)", "Always relevant",
                                              "Only its controller"};
    int current = static_cast<int>(marker->relevance);

    ImGui::TextUnformatted("Relevance");
    ImGui::SameLine(180.f);
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Combo("##relevance", &current, kLabels, IM_ARRAYSIZE(kLabels)))
    {
        // Undoable like any other edit; the sweep at end of frame commits it.
        if (Assisi::Editor::EditHistory *history = ActiveHistory())
        {
            history->RecordBefore(_selectedEntity,
                                  Assisi::Core::Reflect::ComponentIdOf<Assisi::NetSync::Replicated>(),
                                  EditLabel("Relevance", _selectedEntity), _selectedEntity);
        }
        if (Assisi::NetSync::Replicated *mutable_ = _scene->GetMut<Assisi::NetSync::Replicated>(_selectedEntity))
            mutable_->relevance = static_cast<Assisi::NetSync::Relevance>(current);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Always is for anything plot-critical: a radius is a bandwidth tool, not a "
                          "correctness tool. Only-its-controller is for one player's private business, and "
                          "reaches nobody while the entity is uncontrolled.");
    }
}

void EditorApp::DrawReplicationPolicy()
{
    using namespace Assisi::Core::Reflect;

    if (_scene->Get<Assisi::NetSync::Replicated>(_selectedEntity) == nullptr)
        return;

    DrawRelevancePolicy();

    // Rebuilt from the live component set every frame: nothing cached, so adding
    // or removing a component shows up at once, and this list cannot drift from
    // the glyph button on each component header — both read the mask through
    // SelectedEntitySends and write it through SetSelectedEntitySends.
    //
    // Only components the entity actually carries: a checkbox for one it lacks
    // would be asking about something that cannot be sent.
    std::vector<const ComponentMeta *> present;
    for (const ComponentMeta *meta : ComponentRegistry::Instance().ReplicableComponents())
    {
        if (meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation) != nullptr)
            present.push_back(meta);
    }

    // Nothing to list. The "clients get an empty entity" warning above already
    // covers this case, and covers it louder.
    if (present.empty())
        return;

    if (!ImGui::TreeNodeEx("Replicable components", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    for (const ComponentMeta *meta : present)
    {
        const bool gameVeto = IsComponentGameVetoed(*meta);
        bool sends    = SelectedEntitySends(*meta);

        // A component game.json forbids gets a dead switch with a reason, not a
        // live one that silently does nothing.
        ImGui::BeginDisabled(gameVeto);
        if (ImGui::Checkbox(meta->name.c_str(), &sends))
            SetSelectedEntitySends(*meta, sends);
        ImGui::EndDisabled();

        if (gameVeto)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(filtered by game.json)");
        }
    }

    ImGui::TextDisabled("Unticked components stay on this machine.");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The component's header says it *can* replicate; this says whether this entity "
                          "actually sends it. Clients never receive an unticked component, and one already "
                          "delivered is removed from their mirrors.");
    }

    ImGui::TreePop();
}
#endif // ASSISI_NETWORKING

void EditorApp::DrawInstanceInspector()
{
    const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(_selectedInstance);
    if (row == nullptr)
    {
        _selectedInstance = {}; // destroyed while it was selected
        ImGui::TextDisabled("That instance is no longer live.");
        return;
    }

    const bool editable = IsEditable();
    ImGui::BeginDisabled(!editable);

    ImGui::TextUnformatted(row->name.empty() ? "(spawned at runtime)" : row->name.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.65f, 0.85f, 0.65f, 1.f});
    ImGui::TextUnformatted(row->source.c_str());
    ImGui::PopStyleColor();

    const std::size_t memberCount = Assisi::Runtime::MembersOf(*_scene, _selectedInstance).size();
    ImGui::TextDisabled("instance of a blueprint - %zu live member(s)", memberCount);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("This is the copy's placement, not an entity. Editing it moves every member and "
                          "records no overrides — expand the instance in the Entities panel to edit one "
                          "member.");
    }

    ImGui::Separator();
    ImGui::SeparatorText("Placement");

    // Re-read from the row every frame, never cached: the gizmo writes the same
    // fields, and a cached copy would fight it for a frame on every drag.
    glm::vec3 position = row->transform.position;
    glm::vec3 euler    = glm::degrees(glm::eulerAngles(row->transform.rotation));
    float scale    = row->transform.scale.x;

    bool edited = false;
    edited |= ImGui::DragFloat3("position", &position.x, 0.01f);
    edited |= ImGui::DragFloat3("rotation", &euler.x, 0.5f);
    // One number, because uniform scale is all an instance may have. Three would
    // let a non-uniform scale be typed and then silently averaged, which reads as
    // the editor ignoring what was entered.
    edited |= ImGui::DragFloat("scale", &scale, 0.01f, kMinTypedInstanceScale, 0.f, "%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uniform: an instance may translate, rotate, or scale evenly, and nothing else.");

    // Any of the three drags still held keeps the gesture open. They are one
    // control to the author, so IsItemActive — which speaks only for the scale
    // drag, the last item drawn — would not do. Being broad is the safe error
    // here: an unrelated held widget only delays the commit, while missing the
    // hold splits one drag into a transaction per frame.
    if (ImGui::IsAnyItemActive())
    {
        _instanceGesture.Hold();
        _captureEditingActive = true;
    }
    if (edited)
    {
        BeginInstanceGesture(_selectedInstance);
        _instanceGesture.Hold();
        _captureEditingActive = true;

        Assisi::Runtime::Transform placement;
        placement.position = position;
        placement.rotation = glm::normalize(glm::quat(glm::radians(euler)));
        placement.scale    = glm::vec3(scale);
        ApplyInstancePlacement(_selectedInstance, placement);
    }
    // No close here: SweepInstanceGesture owns that, after every panel has had its
    // say. This panel especially must not close its own — it early-returns when
    // the selection goes away, so the close would be skipped on exactly the frame
    // a mid-drag deselect made it matter.

    if (!row->overrides.empty() || !row->removed.empty())
    {
        ImGui::Separator();
        ImGui::TextDisabled("%zu member(s) overridden, %zu removed", row->overrides.size(),
                            row->removed.size());
    }

    ImGui::EndDisabled();

    if (!editable && ImGui::IsWindowHovered())
        ImGui::SetTooltip("This world is inspect-only.");
}

void EditorApp::DrawInspector()
{
    using namespace Assisi::Core::Reflect;

    ImGui::Begin("Inspector");

    // An instance is selected, not an entity. Expansion leaves only members, no
    // root entity, so what there is to edit is the *record* — where the copy
    // stands. Without this branch the panel reads "no entity selected" over a
    // perfectly good selection and the placement can only be dragged, never typed.
    if (_selectedEntity == Assisi::ECS::NullEntity && _selectedInstance.IsValid())
    {
        DrawInstanceInspector();
        ImGui::End();
        return;
    }

    if (_selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity))
    {
        ImGui::TextDisabled("No entity selected.");
        ImGui::TextDisabled("Left-click an object in the scene.");
        ImGui::End();
        return;
    }

    // A resident world that is not the edited one is inspect-only, and the whole
    // panel is disabled rather than parts of it: an edit here could be neither
    // captured (histories bind the edited world) nor saved. A mirror is disabled
    // for a different reason with the same answer — the host owns it.
    const bool mirrored = IsMirrored(_selectedEntity);
    const bool editable = IsEditable(_selectedEntity);
    ImGui::BeginDisabled(!editable);

    ImGui::TextUnformatted(DescribeEntity(_selectedEntity).c_str());

    // A blueprint member's header: which blueprint, which member, which instance,
    // and a way back to the instance. Without it a member reads as an ordinary
    // entity right up until a save turns the edit into an override.
    if (const auto *tag = _scene->Get<Assisi::ECS::BlueprintMember>(_selectedEntity))
    {
        if (const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(tag->instanceId))
        {
            const Assisi::Runtime::BlueprintResult definition =
                Assisi::Runtime::GetBlueprintDefinition(row->source);
            const std::string memberPath =
                definition && tag->memberIndex < (*definition)->members.size()
                    ? (*definition)->members[tag->memberIndex].name
                    : std::format("#{}", tag->memberIndex);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.65f, 0.85f, 0.65f, 1.f});
            ImGui::Text("%s of %s", memberPath.c_str(), row->source.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Editing a field here records an override on this instance — the fields you "
                                  "do not touch keep following the blueprint.");
            }

            if (ImGui::SmallButton("Select instance"))
            {
                _selectedInstance = tag->instanceId;
                _selectedEntity   = Assisi::ECS::NullEntity;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", row->name.empty() ? "(spawned at runtime)" : row->name.c_str());
        }
    }

#if defined(ASSISI_NETWORKING)
    DrawReplicationSection(mirrored);
#else
    (void)mirrored; // no replication block to draw; `mirrored` is always false
#endif

    // The rename box, shown for every entity. It reads the optional Name component
    // and creates one on the first edit, so naming is click-and-type with no "add
    // component" step. An empty name leaves the entity showing its id in the list.
    {
        const Assisi::Runtime::Name *nameComp = _scene->Get<Assisi::Runtime::Name>(_selectedEntity);

        // Opened before the box is drawn, so the first keystroke's *creation* of
        // the Name component is captured as absent -> present. The sweep at end of
        // frame closes it.
        //
        // Never for a mirror. The disabled widget makes the edit impossible and
        // the sweep would drop the no-op anyway, but "a mirror never enters edit
        // history" is a rule worth stating where it applies.
        if (Assisi::Editor::EditHistory *history = editable ? ActiveHistory() : nullptr)
            history->RecordBefore(_selectedEntity,
                                  Assisi::Core::Reflect::ComponentIdOf<Assisi::Runtime::Name>(),
                                  EditLabel("Rename", _selectedEntity), _selectedEntity);

        char nameBuf[Assisi::Core::kEntityNameMax + 1] = {};
        if (nameComp != nullptr)
            nameComp->value.ToCStr(nameBuf, sizeof(nameBuf));
        ImGui::SetNextItemWidth(-1.f);
        const bool edited = ImGui::InputTextWithHint("##entityname", "Name", nameBuf, sizeof(nameBuf));

        // The Rename door (Naming.hpp): refused, not auto-suffixed, because this
        // name was typed on purpose. Checked every frame and written only on a
        // keystroke, so the reason sits under the field while the bad name is
        // still in it. The rule is the loader's, so a name this box accepts is a
        // name the level reloads with.
        const std::string_view typed{nameBuf};
        std::string_view refusal;
        if (const auto allowed = Assisi::Runtime::CheckEntityName(*_scene, _selectedEntity, typed);
            !allowed.has_value())
        {
            refusal = Assisi::Runtime::Describe(allowed.error());
        }

        if (edited && refusal.empty())
        {
            // Adds the component if the entity had none, which is what makes
            // naming click-and-type. Empty clears it.
            (void)Assisi::Runtime::RenameEntity(*_scene, _selectedEntity, typed);
        }
        if (!refusal.empty())
        {
            // Repeated every frame the box holds a bad name, not once on the
            // keystroke that made it bad. The box still shows what was typed, so a
            // one-shot message would leave the author staring at a name the entity
            // does not have with no reason given.
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%.*s", static_cast<int>(refusal.size()),
                               refusal.data());
        }
    }
    ImGui::Separator();

    bool anyFieldEdited = false;

    // A delete-confirm is armed for one component of one entity. Drop it when the
    // selection moves rather than carry it to the newly selected entity.
    if (_pendingDeleteEntity != _selectedEntity)
        _pendingDeleteComponent = Assisi::Core::Reflect::kInvalidComponentId;

    // SerializableComponents() already skips ACOMP(transient) id-only components
    // (RigidBody, DestroyTag), which have no getByEntity hook and nothing to edit,
    // so no per-item guard is needed.
    for (const auto *meta : ComponentRegistry::Instance().SerializableComponents())
    {
        // Name belongs to the rename box above, not to this generic list.
        if (meta->name == "Name")
            continue;

        const void *compPtr =
            meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation);

        if (!compPtr)
            continue;

        ImGui::PushID(meta->name.c_str());

        // Per-component delete. X arms a two-step confirm that replaces it with
        // [Delete] [Cancel]; either way the buttons sit left of the header, so the
        // row reads "[X] ComponentName".
        bool deleted = false;
        if (_pendingDeleteComponent == meta->id)
        {
            if (ImGui::SmallButton("Delete"))
            {
                RemoveComponentFromSelected(*meta);
                _pendingDeleteComponent = Assisi::Core::Reflect::kInvalidComponentId;
                deleted                 = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel"))
                _pendingDeleteComponent = Assisi::Core::Reflect::kInvalidComponentId;
        }
        else if (ImGui::SmallButton("X"))
        {
            _pendingDeleteComponent = meta->id;
            _pendingDeleteEntity    = _selectedEntity;
        }
        else if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Remove component");
        }

        if (deleted)
        {
            // The component is gone and compPtr with it; its fields must not draw.
            ImGui::PopID();
            continue;
        }

        ImGui::SameLine();
        // AllowOverlap: a CollapsingHeader spans the full row, so anything drawn
        // over it with SameLine() is visible but never receives the click — the
        // header's hit box swallows it first. The send-toggle glyph below sits
        // exactly there.
        const bool headerOpen = ImGui::CollapsingHeader(
            meta->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

        // The per-component send toggle, on the header. Drawn only on an entity
        // that replicates at all — nothing travels from a local entity, so the
        // glyph's *absence* is itself the answer there.
        //
        // The other half of this control is the "Replicable components" checklist
        // in DrawReplicationPolicy. Neither holds state: both read
        // SelectedEntitySends and write SetSelectedEntitySends, so they are two
        // renderings of one mask, not two copies to keep in step.
#if defined(ASSISI_NETWORKING)
        if (meta->replicable && _scene->Has<Assisi::NetSync::Replicated>(_selectedEntity))
        {
            const bool gameVeto = IsComponentGameVetoed(*meta);
            const bool sends    = SelectedEntitySends(*meta);

            ImGui::SameLine();
            // A button, not a label. The idle frame is transparent so it still
            // reads as a glyph beside the header; hover and active must stay
            // visible, or nothing signals that it can be clicked at all.
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.f, 0.f, 0.f, 0.f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{1.f, 1.f, 1.f, 0.20f});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{1.f, 1.f, 1.f, 0.35f});
            ImGui::PushStyleColor(ImGuiCol_Text, sends ? kWireColor : kWireOffColor);
            ImGui::BeginDisabled(gameVeto || !editable);
            if (ImGui::SmallButton(kWireGlyph))
                SetSelectedEntitySends(*meta, !sends);
            ImGui::EndDisabled();
            ImGui::PopStyleColor(4);

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (gameVeto)
                    ImGui::SetTooltip("Never sent — game.json's neverReplicate list forbids this component type.");
                else if (!editable)
                    ImGui::SetTooltip(sends ? "Sent to clients." : "Withheld — stays on this machine.");
                else if (sends)
                    ImGui::SetTooltip("Sent to clients. Click to withhold it — mirrors that already have it "
                                      "will drop it.");
                else
                    ImGui::SetTooltip("Withheld: stays on this machine. Click to send it.");
            }
        }
#endif // ASSISI_NETWORKING

        // Whether this instance overrides the component. A member's inspector
        // otherwise looks like any other entity's, right up until a save turns the
        // edit into an override. The marking doubles as the cheap substitute for
        // an override validator: a component shown as overridden that nobody
        // touched is visible at once.
        if (const nlohmann::json *claim = OverrideClaimFor(_selectedEntity, meta->name))
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.f, 0.82f, 0.4f, 1.f});
            ImGui::TextUnformatted(claim->is_null() ? "removed" : "overridden");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("This instance changed %s. Fields it did not change still follow the "
                                  "blueprint.",
                                  claim->is_null() ? "whether it has this component" : "some of these fields");
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(!editable);
            if (ImGui::SmallButton("Reset##component"))
                _pendingOverrideReset = PendingOverrideReset{_selectedEntity, meta->name, /*field=*/ {}};
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Drop this instance's claim and follow the blueprint again.");
        }

        if (headerOpen)
        {
            // Opens the gesture: snapshot the component's pre-edit JSON before the
            // widgets below write it in place. Idempotent across a drag, and the
            // end-of-frame sweep commits or drops it — see EditHistory's
            // record-before-write capture section. The gizmo opens this same
            // (entity, Transform) gesture, so a gizmo drag and an inspector
            // Transform edit coalesce into one transaction rather than two.
            // Never for a mirror — see the rename box above.
            if (Assisi::Editor::EditHistory *history = editable ? ActiveHistory() : nullptr)
                history->RecordBefore(_selectedEntity, meta->id, EditLabel("Edit " + meta->name, _selectedEntity),
                                      _selectedEntity);

            // Only worth saying on an entity that is *trying* to replicate: on a
            // local one every component is unreplicated, so this would be noise on
            // every row.
#if defined(ASSISI_NETWORKING)
            if (!meta->replicable && _scene->Has<Assisi::NetSync::Replicated>(_selectedEntity))
            {
                ImGui::TextDisabled("not replicated — type lacks ACOMP(replicable)");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Clients will never see this component's values. Replication is opt-in "
                                      "per type, in the component's header, not per entity.");
                }
            }
#endif // ASSISI_NETWORKING

            const bool edited = EditComponentFields(const_cast<void *>(compPtr), *meta);
            // The field widgets write component memory by offset, bypassing
            // Scene::GetMut's change stamping, so the change is reported by hand.
            // A no-op for untracked components; for a tracked one (Transform) this
            // is what re-propagates the edit.
            if (edited)
                _scene->MarkChanged(_selectedEntity, meta->id);
            anyFieldEdited |= edited;
        }
        ImGui::PopID();
    }

    // Apply a reset requested above, now that the component loop's pointers are
    // dead anyway. Never at the click: the reset swaps the component out from
    // under the loop and out from under the frame's open capture gesture.
    if (_pendingOverrideReset.has_value())
    {
        const PendingOverrideReset request = *_pendingOverrideReset;
        _pendingOverrideReset.reset();
        ResetOverride(request.entity, request.component, request.field);
    }

    // A typed asset-id edit changes mesh/materialOverrides; re-resolve so the new
    // asset shows without a level reload. Cheap: the AssetCache Resolve* calls are
    // cached lookups. Browser picks re-resolve in SelectAsset instead.
    if (anyFieldEdited)
        ReresolveEntityAssets(_selectedEntity);

    // RigidBody is a runtime handle with no reflected fields, so the loop above
    // cannot show its live simulation state. Read-only, because it changes every
    // physics step.
    if (const auto *rbc = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity))
    {
        if (ImGui::CollapsingHeader("RigidBody (runtime)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const auto [linearVelocity, angularVelocity] = _physics->GetBodyVelocity(*rbc);
            // %f takes a double through varargs, so each float is promoted
            // regardless; the casts only make that explicit.
            ImGui::Text("Linear  (m/s):   %.3f, %.3f, %.3f", static_cast<double>(linearVelocity.x),
                        static_cast<double>(linearVelocity.y), static_cast<double>(linearVelocity.z));
            ImGui::Text("Angular (rad/s): %.3f, %.3f, %.3f", static_cast<double>(angularVelocity.x),
                        static_cast<double>(angularVelocity.y), static_cast<double>(angularVelocity.z));
            ImGui::Text("Speed:  %.3f m/s", static_cast<double>(glm::length(linearVelocity)));
            ImGui::Text("CCD:    %s", _physics->IsBodyCCDEnabled(*rbc) ? "LinearCast (on)" : "Discrete (off)");
        }
    }

    // --- Add Component ---------------------------------------------------------
    ImGui::Separator();
    ImGui::TextUnformatted("Add Component");
    ImGui::SetNextItemWidth(-1.f);

    // Keyboard navigation of the suggestion list. A single-line InputText swallows
    // these keys, so the only way to see them is its callbacks: Tab arrives as
    // Completion, the arrows as History, any text change as Edit. The callback
    // records intent only; the highlight moves below, once this frame's match
    // count is known.
    struct SuggestionNav
    {
        int32_t move  = 0;     // -1 = up, +1 = down (Tab or arrows)
        bool reset = false;    // text edited -> snap back to the first row
    };
    SuggestionNav nav;
    const auto navCallback = [](ImGuiInputTextCallbackData *data) -> int
                             {
                                 auto *n = static_cast<SuggestionNav *>(data->UserData);
                                 switch (data->EventFlag)
                                 {
                                 case ImGuiInputTextFlags_CallbackCompletion: n->move = +1; break; // Tab
                                 case ImGuiInputTextFlags_CallbackHistory:
                                     n->move = data->EventKey == ImGuiKey_UpArrow ? -1 : +1; // Up / Down
                                     break;
                                 case ImGuiInputTextFlags_CallbackEdit: n->reset = true; break; // typed or deleted
                                 default: break;
                                 }
                                 return 0;
                             };

    const bool entered =
        ImGui::InputText("##addcomponent", _addComponentBuf, sizeof(_addComponentBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                         ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit,
                         navCallback, &nav);

    const auto toLower = [](std::string text)
                         {
                             std::transform(text.begin(), text.end(), text.begin(),
                                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                             return text;
                         };
    const std::string queryLower = toLower(_addComponentBuf);

    if (queryLower.empty())
    {
        _addComponentSelected = 0; // nothing to highlight until the field has text
    }
    else
    {
        // Addable components (serializable, not already on the entity) whose
        // lowercased name contains the query, ranked by match position first so a
        // prefix wins, then by name length, then alphabetically. Recomputed every
        // frame, so it tracks each keystroke and deletion.
        struct Match
        {
            const ComponentMeta *meta;
            std::size_t pos;
        };
        std::vector<Match> matches;
        for (const ComponentMeta *meta : ComponentRegistry::Instance().SerializableComponents())
        {
            if (meta->name == "Name") // owned by the rename box, never added here
                continue;
            if (meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation) != nullptr)
                continue;
            const std::string nameLower = toLower(meta->name);
            const std::size_t pos       = nameLower.find(queryLower);
            if (pos == std::string::npos)
                continue;
            matches.push_back(Match{meta, pos});
        }
        std::sort(matches.begin(), matches.end(),
                  [](const Match &a, const Match &b)
            {
                if (a.pos != b.pos)
                    return a.pos < b.pos;
                if (a.meta->name.size() != b.meta->name.size())
                    return a.meta->name.size() < b.meta->name.size();
                return a.meta->name < b.meta->name;
            });

        constexpr std::size_t kMaxSuggestions = 8;
        const std::size_t shown           = std::min(matches.size(), kMaxSuggestions);

        // Resolve this frame's navigation into the highlight index: an edit snaps
        // it to the top, Tab/arrows step it with wrap-around across the shown rows.
        if (nav.reset)
            _addComponentSelected = 0;
        if (shown == 0)
        {
            _addComponentSelected = 0;
            ImGui::TextDisabled("(no matching component)");
        }
        else
        {
            if (nav.move != 0)
                _addComponentSelected =
                    (_addComponentSelected + nav.move + static_cast<int32_t>(shown)) % static_cast<int32_t>(shown);
            _addComponentSelected = std::clamp(_addComponentSelected, 0, static_cast<int32_t>(shown) - 1);

            // Enter adds the highlighted row; clicking a row adds it directly.
            if (entered)
            {
                AddComponentToSelected(*matches[static_cast<std::size_t>(_addComponentSelected)].meta);
                _addComponentBuf[0]   = '\0';
                _addComponentSelected = 0;
                // -1 re-focuses the previous widget, the input itself, so the
                // author can add another component without re-clicking it.
                ImGui::SetKeyboardFocusHere(-1);
            }
            else
            {
                for (std::size_t i = 0; i < shown; ++i)
                {
                    ImGui::PushID(static_cast<int32_t>(i));
                    if (ImGui::Selectable(matches[i].meta->name.c_str(),
                                          static_cast<int32_t>(i) == _addComponentSelected))
                    {
                        AddComponentToSelected(*matches[i].meta);
                        _addComponentBuf[0]   = '\0';
                        _addComponentSelected = 0;
                    }
                    ImGui::PopID();
                }
            }
        }
    }

    // Feeds the end-of-frame capture sweep: while an inspector widget is being
    // manipulated (a drag held, a text field focused) its gesture must stay open
    // until release. Scoped to this window and its children so activity elsewhere
    // — the AA combo, the Save-As field — cannot hold a component gesture open.
    // Same shape as the freeze request in HandlePhysicsEditing, computed
    // independently of it.
    if (ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        _captureEditingActive = true;

    ImGui::EndDisabled();
    if (editable)
    {
        HandlePhysicsEditing(anyFieldEdited);
    }
    ImGui::End();
}

} // namespace Assisi::Editor
