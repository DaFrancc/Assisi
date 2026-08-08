/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
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
/// The same glyph, withheld: dim enough to read as "off" at a glance without
/// becoming invisible, since its absence already means something different
/// (the entity does not replicate at all).
constexpr ImVec4 kWireOffColor{0.42f, 0.42f, 0.46f, 1.f};
/// A plug, from the editor's Nerd Font. Small, and not a word, so it does not
/// compete with the component name it sits beside.
constexpr const char *kWireGlyph = "\xef\x87\xa6"; // U+F1E6
} // namespace

namespace
{

/// @brief Editor visibility of an AFIELD(radio) listener for the current data.
enum class RadioVisibility
{
    Active, ///< Source enum is at one of the field's values — edit normally.
    Greyed, ///< Not active; show disabled (radioBehavior = grey).
    Hidden, ///< Not active; omit entirely (radioBehavior = vanish).
};

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

/// @brief Read a reflected enum's value from `fp` at its true underlying width.
///
/// A reflected enum may have any fixed-width underlying (1/2/4/8 bytes), so it
/// must be read at that exact width — treating an 8/16-bit enum as a 4-byte int
/// reads (and a write would clobber) neighbouring bytes. `signed_` sign-extends.
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

/// @brief Write `value` into a reflected enum at `fp` at its underlying width.
/// Truncation is the same bit pattern for signed/unsigned, so no sign flag.
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

/// @brief Resolve a field's radio state against a live component instance.
///
/// A listener (radioSource set) follows a sibling broadcaster enum. Because a
/// source may itself be a listener, this walks the source chain up to the root
/// broadcaster and folds back down: while any source in the chain is inactive,
/// this field hides unconditionally; once every source above is active, the
/// field is Active if its own source's current value is one of radioValues,
/// otherwise it applies its own radioBehavior. Non-listener fields are always
/// Active. reflectgen validates the chain exists and is acyclic, so the bounded
/// walk and the lookup misses handled here are purely defensive.
RadioVisibility EvaluateRadio(const void *component, const Assisi::Core::Reflect::ComponentMeta &meta,
                              const Assisi::Core::Reflect::FieldMeta &field)
{
    using Assisi::Core::Reflect::FieldMeta;
    using Assisi::Core::Reflect::RadioBehavior;

    if (field.radioSource.empty())
    {
        return RadioVisibility::Active;
    }

    // Collect the chain: field, its source, its source's source, ... up to the
    // root broadcaster (a field with no radioSource). Bounded by the field count
    // since the graph is acyclic.
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

    const auto readEnum = [component](const FieldMeta *fm) -> std::int64_t
    {
        const void *fp = static_cast<const char *>(component) + fm->offset;
        return ReadEnumValue(fp, fm->enumSize, fm->enumSigned);
    };

    // Fold from the root down. `state` holds the resolved visibility of the source
    // one level up as we descend toward `field` (chain front).
    RadioVisibility state = RadioVisibility::Active; // the root broadcaster
    for (std::size_t i = chain.size(); i-- > 1;)
    {
        const FieldMeta *listener = chain[i - 1];
        const FieldMeta *source   = chain[i];
        if (state != RadioVisibility::Active)
        {
            state = RadioVisibility::Hidden; // an inactive source hides its listeners outright
            continue;
        }
        const std::int64_t current = readEnum(source);
        bool               match   = false;
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

        // AFIELD(norep): saved to disk like anything else, never sent. Dimmed
        // rather than disabled — it is authored data, and the author is the one
        // person who *should* be editing it; what they need is to know it stays
        // here.
        const bool serverOnly = field.norep;
        if (serverOnly)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        // AFIELD(radio) listeners follow a sibling enum: hide or grey them when
        // that enum isn't at one of the field's active values.
        const RadioVisibility radio = EvaluateRadio(mut, meta, field);
        if (radio == RadioVisibility::Hidden)
        {
            continue;
        }
        const bool greyed = (radio == RadioVisibility::Greyed);

        void *fp = static_cast<char *>(mut) + field.offset;
        ImGui::PushID(field.name.c_str());
        if (greyed)
        {
            ImGui::BeginDisabled();
        }

        bool edited = false;
        switch (field.type)
        {
        case FieldType::Float:
        {
            // AFIELD(min=/max=) editor hints. DragFloat treats min==max==0 as
            // "no clamp", so open sides substitute ±FLT_MAX; AlwaysClamp also
            // covers Ctrl+click text entry, which ignores plain drag bounds.
            const float minBound = field.hasMin ? field.minValue : -FLT_MAX;
            const float maxBound = field.hasMax ? field.maxValue : FLT_MAX;
            edited = ImGui::DragFloat(field.name.c_str(), static_cast<float *>(fp), 0.01f, minBound, maxBound,
                                      "%.3f", ImGuiSliderFlags_AlwaysClamp);
            break;
        }
        case FieldType::Double:
        {
            edited = ImGui::InputDouble(field.name.c_str(), static_cast<double *>(fp));
            // InputDouble has no clamp support, so enforce the bounds after the
            // edit instead.
            if (edited)
            {
                double &value = *static_cast<double *>(fp);
                if (field.hasMin)
                {
                    value = std::max(value, static_cast<double>(field.minValue));
                }
                if (field.hasMax)
                {
                    value = std::min(value, static_cast<double>(field.maxValue));
                }
            }
            break;
        }
        case FieldType::Int32:
        {
            // reflectgen guarantees integer-field bounds are integral and in
            // range, so the casts below are exact.
            const int32_t minBound = field.hasMin ? static_cast<int32_t>(field.minValue) : INT32_MIN;
            const int32_t maxBound = field.hasMax ? static_cast<int32_t>(field.maxValue) : INT32_MAX;
            edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_S32, fp, 1.f, &minBound, &maxBound,
                                       nullptr, ImGuiSliderFlags_AlwaysClamp);
            break;
        }
        case FieldType::UInt32:
        {
            const uint32_t minBound = field.hasMin ? static_cast<uint32_t>(field.minValue) : 0u;
            const uint32_t maxBound = field.hasMax ? static_cast<uint32_t>(field.maxValue) : UINT32_MAX;
            edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_U32, fp, 1.f, &minBound, &maxBound,
                                       nullptr, ImGuiSliderFlags_AlwaysClamp);
            break;
        }
        case FieldType::Int64:
        {
            // Bounds arrive as double, which holds integers exactly only to 2^53;
            // past that a bound would silently round, so clamp to the exactly
            // representable range rather than pretend to honour it.
            constexpr int64_t kExact   = 1LL << 53;
            const int64_t     minBound = field.hasMin ? static_cast<int64_t>(field.minValue) : -kExact;
            const int64_t     maxBound = field.hasMax ? static_cast<int64_t>(field.maxValue) : kExact;
            edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_S64, fp, 1.f, &minBound, &maxBound,
                                       nullptr, ImGuiSliderFlags_AlwaysClamp);
            break;
        }
        case FieldType::UInt64:
        {
            constexpr uint64_t kExact   = 1ULL << 53;
            const uint64_t     minBound = field.hasMin ? static_cast<uint64_t>(field.minValue) : 0u;
            const uint64_t     maxBound = field.hasMax ? static_cast<uint64_t>(field.maxValue) : kExact;
            edited = ImGui::DragScalar(field.name.c_str(), ImGuiDataType_U64, fp, 1.f, &minBound, &maxBound,
                                       nullptr, ImGuiSliderFlags_AlwaysClamp);
            break;
        }
        case FieldType::Bool:
            edited = ImGui::Checkbox(field.name.c_str(), static_cast<bool *>(fp));
            break;
        case FieldType::Enum:
        {
            // Enums are stored as their underlying integer, whose width varies
            // (see AENUM); read/write it at field.enumSize so an 8/16-bit enum
            // isn't clobbered. Show the enumerators as a dropdown.
            const std::int64_t value = ReadEnumValue(fp, field.enumSize, field.enumSigned);
            const char        *preview = "(unknown)";
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
            // FieldType::String is Core::ShortString (the only String type today).
            auto *str = static_cast<Assisi::Core::ShortString *>(fp);
            char  buf[Assisi::Core::kShortStringMax + 1];
            str->ToCStr(buf, sizeof(buf));
            if (ImGui::InputText(field.name.c_str(), buf, sizeof(buf)))
            {
                str->Assign(buf);
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
        case FieldType::Quat:
        {
            auto     *quat  = static_cast<glm::quat *>(fp);
            glm::vec3 euler = glm::degrees(glm::eulerAngles(*quat));
            if (ImGui::DragFloat3(field.name.c_str(), &euler.x, 0.5f))
            {
                *quat  = glm::normalize(glm::quat(glm::radians(euler)));
                edited = true;
            }
            break;
        }
        case FieldType::AssetPath:
        {
            auto *ap = static_cast<Assisi::Core::AssetPath *>(fp);
            char  buf[Assisi::Core::kAssetPathMax + 1];
            ap->ToCStr(buf, sizeof(buf));

            // Text field + a browse button that opens the asset browser for this
            // field. Hide the input's own label so we can lay the button, then the
            // visible label, out to its right: [ input ][…] fieldName
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
            // Stored as a GUID; shown/edited as its resolved virtual path (the
            // database translates both ways). Same [ input ][…] label layout as
            // AssetPath.
            auto             *id      = static_cast<Assisi::Core::AssetId *>(fp);
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
            auto      *ref   = static_cast<Assisi::ECS::Entity *>(fp);
            const bool empty = (*ref == Assisi::ECS::NullEntity);

            std::string preview;
            if (empty)
                preview = "(none)";
            else if (!_scene->IsAlive(*ref))
                preview = std::format("[{}:{}] (dangling)", ref->index, ref->generation);
            else
                preview = DescribeEntity(*ref);

            const bool  armedForThis = _eyedropperArmed && _eyedropperMeta == &meta &&
                                      _eyedropperFieldOffset == field.offset;
            const char *pickLabel    = armedForThis ? "Cancel" : "Pick";

            /* Eyedropper button first so it can't be pushed off the window's right
               edge by the combo's trailing label; the combo takes the rest. */
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
                        const bool        selected = (e == *ref);
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
            // Only MeshRenderer::materialOverrides uses this type; render it as
            // one browse row per material slot of the entity's resolved mesh.
            if (meta.name == "MeshRenderer" && field.name == "materialOverrides")
                edited = EditMaterialSlots(*static_cast<Assisi::Runtime::MeshRenderer *>(mut), meta, field.offset);
            else
                ImGui::TextDisabled("%s: [unsupported vector]", field.name.c_str());
            break;
        }
        default:
            ImGui::TextDisabled("%s: [unsupported type]", field.name.c_str());
            break;
        }

        if (greyed)
        {
            ImGui::EndDisabled();
        }
        if (serverOnly)
        {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("(server-only)");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("AFIELD(norep): saved with the level, never sent to clients. Every client "
                                  "holds this field's default.");
            }
        }
        // A dot beside the fields this instance actually claims, and a right-click
        // to drop the claim. Per field rather than per component because that is
        // the granularity the record has: resetting one field leaves the others
        // following the blueprint, which is the whole point of the merge rule.
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
                // Queued, not applied: the reset removes and re-adds the
                // component, and this loop is still walking a pointer into the
                // pool that would move underneath it. Drained once the loop ends.
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

        // Current override for this slot (nil id when the slot falls back to the
        // mesh's imported default), shown/edited as its resolved path.
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

        // Label by the mesh's imported material name, falling back to the index.
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
    // Display an id as its current virtual path; typing a path re-resolves the id
    // through the database (nil when the path is unknown). The caller lays out the
    // browse button and label to the right, so this only sizes+draws the input.
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

    // IsAnyItemActive() is global — it fires for a drag/edit in *any* window, so
    // touching the AA combo or the Save-As field would otherwise freeze the
    // selected body to Static. Scope it to the Inspector: this runs inside the
    // Inspector's Begin/End, so IsWindowFocused (current window) is true only
    // while an Inspector widget is the one being manipulated.
    const bool nowDragging =
        ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (nowDragging)
        RequestPhysicsFreeze(); // the release is handled at end of frame, not here
}

void EditorApp::RequestPhysicsFreeze()
{
    // Raise the request every frame the gesture is held. OnImGui thaws whatever is
    // frozen on the first frame this is *not* raised, so the release survives the
    // caller not running at all (nothing selected, entity destroyed mid-drag, the
    // world selector moved to an inspect-only world).
    _physicsFreezeRequested = true;

    if (_frozenBodyEntity == _selectedEntity)
        return; // already held, nothing to do

    // Selection moved while a gesture was live: the previous body must be released
    // before this one is taken, or it stays Static with a descriptor that still
    // says dynamic — a body stuck in mid-air that nothing in the UI explains.
    if (_frozenBodyEntity != Assisi::ECS::NullEntity)
        ThawEditedBody();

    // Everything below goes through _world rather than the _scene/_physics
    // shortcuts, so the hold is taken out of exactly the world the release will
    // look up by name. They track the same world today; not depending on that is
    // cheaper than the class of bug it would otherwise invite.
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
    const std::string         worldName = _frozenBodyWorld;

    // Cleared up front: every path below returns having released the freeze, and
    // the ones that bail (world gone, entity destroyed) must not leave a record
    // pointing at something that no longer exists.
    _frozenBodyEntity = Assisi::ECS::NullEntity;
    _frozenBodyWorld.clear();

    // By name, not by the app's current _scene/_physics: the viewed world can have
    // changed since the freeze, and thawing against the wrong world would leave
    // the real body frozen while poking an unrelated one.
    Assisi::App::World *world = _worlds.Find(worldName);
    if (world == nullptr || !world->scene.IsAlive(entity))
        return; // world or entity is gone; its body went with it

    const auto *rbc = world->scene.Get<Assisi::Physics::RigidBody>(entity);
    if (rbc == nullptr)
        return; // collider removed during the drag

    // Back to whatever the descriptor authored, not unconditionally Dynamic — the
    // freeze must be invisible, including for bodies that were Static all along.
    const auto *desc     = world->scene.Get<Assisi::Physics::RigidBodyDescriptor>(entity);
    const bool  isStatic = desc && desc->isStatic;
    world->physics.SetBodyMotionType(*rbc, isStatic ? Assisi::Physics::BodyMotion::Static
                                                    : Assisi::Physics::BodyMotion::Dynamic);
    if (!isStatic)
    {
        // Land the body on wherever the drag left the Transform, so it resumes
        // simulating from the pose the author sees rather than the pre-drag one.
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
    // Adding one the entity already has would overwrite it — skip (the Add
    // Component field also filters these out of its suggestions).
    if (meta.getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation) != nullptr)
    {
        return;
    }

    // Capture as one transaction: record the absent-before state, then commit after
    // the add *and* its side effects below (camera-facing placement, physics body),
    // so undo restores exactly what the author sees.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, meta.id, EditLabel("Add " + meta.name, _selectedEntity),
                              _selectedEntity);

    // Default-construct it: an empty JSON object leaves every field at its default
    // via the per-field if-contains deserialization — the same path the level
    // loader uses, so no component needs a bespoke "make default" hook.
    meta.addToScene(_scene, _selectedEntity.index, _selectedEntity.generation, nlohmann::json::object());

    // A couple of components carry runtime state beyond their reflected fields;
    // wire it up so the add takes effect immediately rather than at next reload.
    if (meta.name == "Transform")
    {
        // Entities start transform-less; when the author gives one a placement,
        // drop it in front of the camera so it lands in view rather than at the
        // world origin. GetMut, not Get: Transform is ACOMP(tracked), and the Add
        // above stamping is not enough on its own — that stamp covers the default
        // pose, this write replaces it, and a consumer must see the tick for the
        // value it will actually read.
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

    // Capture the present-before state so undo can restore the removed component.
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, meta.id, EditLabel("Remove " + meta.name, _selectedEntity),
                              _selectedEntity);

    // Tear down runtime state that lives outside the reflected fields before the
    // pool entry disappears. A RigidBodyDescriptor owns a Jolt body (via the
    // transient RigidBody handle); drop both so the collider stops simulating.
    // MeshRenderer's transient pointers are non-owning (the AssetCache owns the
    // GPU resources), so removing it needs no extra cleanup.
    // Transform is included: a body's pose is driven from it, so removing the
    // Transform while a RigidBodyDescriptor remains would leave a live Jolt body
    // simulating with nothing to sync it — an orphan that only a level reload
    // clears. The descriptor itself survives, matching what removing the
    // descriptor does to the transient handle.
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

void EditorApp::DrawReplicationSection(bool mirrored)
{
    using namespace Assisi::Core::Reflect;

    // Presence of the marker is the gate, and it is the *only* gate — there is
    // deliberately no checkbox here duplicating it. The marker is an ordinary
    // component, so it is added and removed through the same Add Component menu
    // and the same undoable path as anything else; a second control that meant
    // the same thing would be a second place for the two to disagree.
    const bool isReplicated = _scene->Has<Assisi::NetSync::Replicated>(_selectedEntity);

    // The session-scoped identity, on whichever side is looking. Worth showing
    // because it is the only name the two machines share — an entity handle is
    // meaningless across a connection, so "which one is this on the other
    // screen" has no other answer.
    if (_netSession != nullptr && _netSession->IsActive())
    {
        Assisi::NetSync::NetId netId = Assisi::NetSync::InvalidNetId;
        if (const Assisi::NetSync::ReplicationServer *server = _netSession->Server())
            netId = server->NetIdOf(_selectedEntity);
        else if (const Assisi::NetSync::ReplicationClient *client = _netSession->Client())
            netId = client->NetIdOf(_selectedEntity);

        if (netId != Assisi::NetSync::InvalidNetId)
            ImGui::TextDisabled("NetId %u", netId.value); // printf boundary
    }

    if (mirrored)
    {
        ImGui::TextColored(ImVec4{0.55f, 0.75f, 1.f, 1.f}, "Mirrored — the host owns this entity.");

        // Which of the two client timelines this entity is on. The
        // discriminator is a replicated RigidBodyDescriptor, which is invisible
        // in the world, so "why do these two lag differently" would otherwise
        // cost a debugging session instead of a glance.
        const bool bodied = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity) != nullptr;
        ImGui::TextDisabled("Replication path: %s", bodied ? "body-corrected" : "interpolated");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(bodied ? "Simulated locally and re-anchored by the host's corrections. Renders at "
                                       "host time minus transit."
                                     : "No local simulation, so it is interpolated between received snapshots — "
                                       "about two snapshot intervals behind.");
        }

        // What this mirror is actually receiving, derived from the components
        // that arrived — **not** from its local Replicated marker.
        //
        // That distinction matters and is easy to get wrong: a mirror's marker is
        // default-constructed by the client when it creates the entity, and the
        // host's real one was stripped from the joined world. Rendering its
        // exclusion mask would display fabricated data — an always-empty policy —
        // dressed up as the host's. Observed presence is the only thing a client
        // truthfully knows.
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

        // Who controls it, if anyone. Read from the replicated component, which
        // is an observed fact rather than a fabrication — unlike this mirror's
        // Replicated marker, which the client default-constructed.
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
    // Every warning here is about a mismatch between what the author marked and
    // what the wire will do with it, and every one of them is otherwise found by
    // watching a client and wondering.

    // Nothing to send. Transform is replicated and almost everything has one, so
    // this fires on genuinely empty entities — which look fine locally.
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

    // Hierarchy is not replicated in v1 (the EntityRef machinery works; the
    // *semantics* — mirrored children of local parents, transform spaces, world
    // -space body state under a parent-relative Transform — are unsolved). So a
    // marked entity in a hierarchy loses the hierarchy, on both ends.
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

    // Excluding Transform is legal — a pure-data entity carrying replicated
    // game state has no spatial meaning to send — but on an entity that is
    // *placed*, it produces a mirror stuck at whatever pose the level file had.
    // The server honours the authored policy either way; this says why the
    // result will look wrong, rather than quietly overriding what someone wrote.
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
    // The capability/policy split, made authorable. Every capable component this
    // entity carries gets a checkbox; unticking one authors an exclusion. The
    // default is everything ticked, which is what an empty mask means.
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

    // Through the undo path like any other edit — a policy change *is* an edit,
    // and Ctrl-Z should mean the same thing here as anywhere else.
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

    // A mirror's Replicated marker is default-constructed by the client when it
    // creates the entity — the host's real one never travels. Rendering its
    // relevance as an authorable dropdown would display fabricated data dressed
    // up as the host's policy, which is the same mistake the Receiving list
    // exists to avoid. Mirrors show observed facts only.
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
        // Through the undo path like any other edit — a policy change *is* an
        // edit, and Ctrl-Z should mean the same thing here as anywhere else.
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

    // Gathered from the live component set every frame, so adding or removing a
    // component is reflected immediately — nothing is cached that could go stale.
    // It is also why this list and the glyph button on each component header
    // cannot disagree: both render the same mask through SelectedEntitySends,
    // and both write it through SetSelectedEntitySends.
    //
    // Only what this entity actually carries: a checkbox for a component it does
    // not have would be asking about something that cannot be sent.
    std::vector<const ComponentMeta *> present;
    for (const ComponentMeta *meta : ComponentRegistry::Instance().ReplicableComponents())
    {
        if (meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation) != nullptr)
            present.push_back(meta);
    }

    // Nothing to list. The empty case already has a louder home — the warning
    // above says clients get an empty entity — so an empty tree node here would
    // only be a second voice saying it worse.
    if (present.empty())
        return;

    if (!ImGui::TreeNodeEx("Replicable components", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    for (const ComponentMeta *meta : present)
    {
        const bool gameVeto = IsComponentGameVetoed(*meta);
        bool       sends    = SelectedEntitySends(*meta);

        // A component the game forbids gets a dead switch with a reason rather
        // than a live one that silently does nothing.
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

void EditorApp::DrawInstanceInspector()
{
    const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(_selectedInstance);
    if (row == nullptr)
    {
        _selectedInstance = {}; // it went away while it was selected
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

    // Read out of the row every frame rather than caching: the gizmo writes the same
    // field, and a cached copy would fight it for a frame on every drag.
    glm::vec3 position = row->transform.position;
    glm::vec3 euler    = glm::degrees(glm::eulerAngles(row->transform.rotation));
    float     scale    = row->transform.scale.x;

    bool edited = false;
    edited |= ImGui::DragFloat3("position", &position.x, 0.01f);
    edited |= ImGui::DragFloat3("rotation", &euler.x, 0.5f);
    // One number, because that is what an instance may have. Three would let a
    // non-uniform scale be typed and then be silently averaged, which reads as the
    // editor ignoring what was entered.
    edited |= ImGui::DragFloat("scale", &scale, 0.01f, kMinTypedInstanceScale, 0.f, "%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Uniform: an instance may translate, rotate, or scale evenly, and nothing else.");

    // Any of the three still held keeps the gesture open; they are one control as
    // far as the author is concerned, so IsItemActive on the scale drag alone (the
    // last item drawn) would not speak for the other two. IsAnyItemActive does, and
    // it is safe to be broad here: holding an unrelated widget only delays the
    // commit by as long as it is held, whereas missing the hold splits one drag
    // into one transaction per frame.
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
    // No close here either — SweepInstanceGesture owns that, after every panel has
    // had its say. This panel in particular must not: it early-returns when the
    // selection goes away, so a close of its own would be skipped on exactly the
    // frame a mid-drag deselect made it matter.

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

    // An instance is selected, not an entity. It has no root to inspect — the root
    // evaporates at expansion (§3) — so what there is to edit is the *record*: where
    // the copy stands. Without this the panel said "no entity selected" over a
    // perfectly good selection, and the placement could only be dragged, never typed.
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

    // A non-edited resident world is inspect-only: its fields read, none of them
    // write. Blanket-disabling the whole panel is deliberate — an edit here could
    // not be captured (the histories bind the edited world) nor saved. A mirror
    // is the same answer for a different reason: the host owns it.
    const bool mirrored = IsMirrored(_selectedEntity);
    const bool editable = IsEditable(_selectedEntity);
    ImGui::BeginDisabled(!editable);

    ImGui::TextUnformatted(DescribeEntity(_selectedEntity).c_str());

    // A member's header block: which blueprint, which member, which instance, and
    // a way back to the instance itself. Without it a member reads as an ordinary
    // entity right up until a save quietly turns the edit into an override.
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

    DrawReplicationSection(mirrored);

    // Rename field: every entity gets an always-available name box. It reads the
    // optional Name component and creates one on first edit, so naming an entity
    // is a single click-and-type — no "add component" step. An empty name leaves
    // the entity showing its id in the list.
    {
        Assisi::Runtime::Name *nameComp = _scene->Get<Assisi::Runtime::Name>(_selectedEntity);

        // Record-before-write for the rename: capture the Name state (present or
        // absent) before the field can change it, so the first-keystroke *creation*
        // of the Name component is captured as absent -> present.
        //
        // Not for a mirror. The disabled widget above already makes the edit
        // impossible and the end-of-frame sweep would drop the no-op anyway, but
        // "a mirror never enters edit history" is a rule worth enforcing where it
        // is stated rather than inferring from two other mechanisms.
        if (Assisi::Editor::EditHistory *history = editable ? ActiveHistory() : nullptr)
            history->RecordBefore(_selectedEntity,
                                  Assisi::Core::Reflect::ComponentIdOf<Assisi::Runtime::Name>(),
                                  EditLabel("Rename", _selectedEntity), _selectedEntity);

        char nameBuf[Assisi::Core::kShortStringMax + 1] = {};
        if (nameComp != nullptr)
            nameComp->value.ToCStr(nameBuf, sizeof(nameBuf));
        ImGui::SetNextItemWidth(-1.f);
        const bool edited = ImGui::InputTextWithHint("##entityname", "Name", nameBuf, sizeof(nameBuf));

        // What the name may be, asked of the same rule the loader enforces — so a
        // name the box accepts is a name the level reloads with. Two entities of
        // one name, or a name spelling `car/body`, both make the loader refuse the
        // file outright: authored happily, lost on reopen (round-7 S17's
        // neighbourhood).
        //
        // Refused rather than auto-suffixed, unlike a *new* entity's name. A fresh
        // entity has no name worth keeping, so stepping it to `Entity_1` costs
        // nothing; a rename is something the author typed on purpose, and quietly
        // storing `crate_1` when they asked for `crate` is the edit they did not
        // make. Empty is not refused — it means "no name", and the entity falls
        // back to its id in the list.
        const std::string_view typed{nameBuf};
        std::string_view       refusal;
        if (!typed.empty())
        {
            if (const auto valid = Assisi::Runtime::ValidateName(typed); !valid.has_value())
                refusal = Assisi::Runtime::Describe(valid.error());
            else if (Assisi::Runtime::EntityNameTaken(*_scene, typed, _selectedEntity))
                refusal = "another entity already has this name";
        }

        if (edited && refusal.empty())
        {
            if (nameComp == nullptr)
                nameComp = _scene->Add<Assisi::Runtime::Name>(_selectedEntity, {});
            if (nameComp != nullptr)
                nameComp->value.Assign(nameBuf);
        }
        if (!refusal.empty())
        {
            // Said every frame the field holds a bad name, not once on the
            // keystroke that made it bad: the box still shows what was typed, so
            // an explanation that had already scrolled away would leave the author
            // looking at a name the entity does not have and no reason why.
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%.*s", static_cast<int>(refusal.size()),
                               refusal.data());
        }
    }
    ImGui::Separator();

    bool anyFieldEdited = false;

    // A delete-confirm is armed for one component of one entity; if the selection
    // moved on, drop it rather than carry it to the newly selected entity.
    if (_pendingDeleteEntity != _selectedEntity)
        _pendingDeleteComponent = Assisi::Core::Reflect::kInvalidComponentId;

    // SerializableComponents() skips ACOMP(transient) id-only components
    // (e.g. RigidBody/DestroyTag), which have no getByEntity hook and nothing
    // to edit, so no per-item guard is needed here.
    for (const auto *meta : ComponentRegistry::Instance().SerializableComponents())
    {
        // Name is edited by the rename field above; don't also list it as a
        // generic component.
        if (meta->name == "Name")
            continue;

        const void *compPtr =
            meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation);

        if (!compPtr)
            continue;

        ImGui::PushID(meta->name.c_str());

        // Per-component delete: the X button arms a two-step confirm that replaces
        // it with [Delete] [Cancel]; both are laid out left of the header so the
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
            // The component (and its stale compPtr) is gone; don't render its fields.
            ImGui::PopID();
            continue;
        }

        ImGui::SameLine();
        // AllowOverlap because a CollapsingHeader spans the full row width, so
        // anything drawn over it with SameLine() is *visible* but never gets the
        // click — the header's hit box swallows it first. The send-toggle glyph
        // below sits exactly there.
        const bool headerOpen = ImGui::CollapsingHeader(
            meta->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

        // The per-component send toggle, on the header where you are already
        // looking. Shown only on an entity that replicates at all: on a local
        // entity nothing travels regardless, so a glyph there would be answering
        // a question nobody asked — and its *absence* is then meaningful.
        //
        // The other half of this control is the "Replicable components" checklist
        // in the replication section. Neither owns any state: both read
        // SelectedEntitySends and write SetSelectedEntitySends, so they are two
        // renderings of one mask rather than two copies to keep in step.
        if (meta->replicable && _scene->Has<Assisi::NetSync::Replicated>(_selectedEntity))
        {
            const bool gameVeto = IsComponentGameVetoed(*meta);
            const bool sends    = SelectedEntitySends(*meta);

            ImGui::SameLine();
            // A button, not a label. Idle frame is transparent so it still reads
            // as a glyph beside the header, but hover and active are visible —
            // without them nothing signals that it can be clicked, which is
            // exactly how the first version of this failed.
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

        // Overridden here? A member's inspector otherwise looks exactly like any
        // other entity's, right up until a save turns the edit into an override —
        // and the marking is also the cheap version of the validator this design
        // skips: a field shown as overridden that you never touched is visible
        // immediately (docs/blueprint-system-concept.md §10).
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
                _pendingOverrideReset = PendingOverrideReset{_selectedEntity, meta->name, /*field=*/{}};
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Drop this instance's claim and follow the blueprint again.");
        }

        if (headerOpen)
        {
            // Record-before-write: snapshot this component's pre-edit JSON before its
            // widgets (which write in-place). Idempotent across a drag; the sweep at
            // end of frame commits or drops it. See EditHistory.hpp §5. The gizmo
            // shares this same (entity, Transform) gesture, so a gizmo drag and an
            // inspector Transform edit are one coalesced transaction, not two.
            // Never for a mirror — see the rename field above.
            if (Assisi::Editor::EditHistory *history = editable ? ActiveHistory() : nullptr)
                history->RecordBefore(_selectedEntity, meta->id, EditLabel("Edit " + meta->name, _selectedEntity),
                                      _selectedEntity);

            // Only worth saying on an entity that is *trying* to replicate; on a
            // local entity every component is unreplicated and the note would be
            // noise on every row.
            if (!meta->replicable && _scene->Has<Assisi::NetSync::Replicated>(_selectedEntity))
            {
                ImGui::TextDisabled("not replicated — type lacks ACOMP(replicable)");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Clients will never see this component's values. Replication is opt-in "
                                      "per type, in the component's header, not per entity.");
                }
            }

            const bool edited = EditComponentFields(const_cast<void *>(compPtr), *meta);
            // Field edits write component memory by offset, bypassing Scene::GetMut's
            // change stamping, so report it explicitly. No-op for untracked
            // components; for a tracked one (Transform) this re-propagates the edit.
            if (edited)
                _scene->MarkChanged(_selectedEntity, meta->id);
            anyFieldEdited |= edited;
        }
        ImGui::PopID();
    }

    // The component loop is done, so its pointers are dead anyway: apply a reset
    // requested above. Here rather than at the click, because the reset swaps the
    // component out from under the loop and out from under the frame's open
    // capture gesture.
    if (_pendingOverrideReset.has_value())
    {
        const PendingOverrideReset request = *_pendingOverrideReset;
        _pendingOverrideReset.reset();
        ResetOverride(request.entity, request.component, request.field);
    }

    // A typed asset-id edit changes mesh/materialOverrides; re-resolve so the
    // new mesh/material shows without a level reload. (Browser picks re-resolve in
    // SelectAsset.) Cheap — the AssetCache Resolve* calls are cached lookups.
    if (anyFieldEdited)
        ReresolveEntityAssets(_selectedEntity);

    // RigidBody is a runtime handle with no reflected fields, so its live
    // simulation state isn't covered by the loop above. Surface the body's
    // velocities read-only (they change every step) for entities that have one.
    if (const auto *rbc = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity))
    {
        if (ImGui::CollapsingHeader("RigidBody (runtime)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const auto [linearVelocity, angularVelocity] = _physics->GetBodyVelocity(*rbc);
            // %f takes a double through varargs, so each float is promoted anyway —
            // the casts just make the promotion explicit rather than implicit.
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

    // Keyboard navigation of the suggestion list, surfaced from inside the focused
    // input: Tab fires the Completion callback, the arrow keys fire History, and any
    // text change fires Edit. We only record the intent here — a single-line
    // InputText exposes these keys nowhere else — and move the highlight below, once
    // this frame's match count is known.
    struct SuggestionNav
    {
        int32_t move  = 0;     // -1 = up, +1 = down (Tab or arrows)
        bool    reset = false; // text edited -> snap back to the first row
    };
    SuggestionNav  nav;
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
        // Rank addable components (serializable, not already on the entity) whose
        // lowercased name contains the query: earliest match position first (so a
        // prefix wins), then shorter name, then alphabetical. Recomputed every
        // frame, so it tracks each keystroke and deletion live.
        struct Match
        {
            const ComponentMeta *meta;
            std::size_t          pos;
        };
        std::vector<Match> matches;
        for (const ComponentMeta *meta : ComponentRegistry::Instance().SerializableComponents())
        {
            if (meta->name == "Name") // managed by the rename field, not added here
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
        const std::size_t     shown           = std::min(matches.size(), kMaxSuggestions);

        // Resolve this frame's navigation into the highlight index: an edit snaps it
        // back to the top; Tab/arrows step it with wrap-around across the shown rows.
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
                // Re-focus the input (the previous widget) so the author can keep
                // typing and add another component without re-clicking the field.
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

    // Feed the capture sweep: while an inspector widget is actively manipulated
    // (a drag held, a text field focused), its edit gesture must stay open until
    // release. Scoped to this window (root+children) so activity elsewhere — the
    // AA combo, the Save-As field — doesn't hold a component gesture open. Same
    // signal shape as HandlePhysicsEditing's _wasDragging, computed independently.
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
