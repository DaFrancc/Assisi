/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/NameComponent.hpp>

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

bool SandboxApp::EditComponentFields(void *mut, const Assisi::Core::Reflect::ComponentMeta &meta)
{
    using namespace Assisi::Core::Reflect;

    bool anyEditable   = false;
    bool anyFieldEdited = false;

    for (const auto &field : meta.fields)
    {
        if (field.transient)
            continue;
        anyEditable = true;

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
        case FieldType::Int:
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

            char preview[32];
            if (empty)
                std::snprintf(preview, sizeof(preview), "(none)");
            else if (!_scene->IsAlive(*ref))
                std::snprintf(preview, sizeof(preview), "[%u:%u] (dangling)", ref->index, ref->generation);
            else
                std::snprintf(preview, sizeof(preview), "Entity [%u:%u]", ref->index, ref->generation);

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
            if (ImGui::BeginCombo(field.name.c_str(), preview))
            {
                if (ImGui::Selectable("(none)", empty))
                {
                    *ref   = Assisi::ECS::NullEntity;
                    edited = true;
                }
                _scene->ForEachEntity(
                    [&](Assisi::ECS::Entity e)
                    {
                        char label[32];
                        std::snprintf(label, sizeof(label), "Entity [%u:%u]", e.index, e.generation);
                        const bool selected = (e == *ref);
                        if (ImGui::Selectable(label, selected))
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
        anyFieldEdited |= edited;
        ImGui::PopID();
    }

    if (!anyEditable)
        ImGui::TextDisabled("(runtime-only)");

    return anyFieldEdited;
}

bool SandboxApp::EditMaterialSlots(Assisi::Runtime::MeshRenderer &mrc,
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

bool SandboxApp::AssetIdPathField(const char *inputId, Assisi::Core::AssetId &id)
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

void SandboxApp::HandlePhysicsEditing(bool anyFieldEdited)
{
    if (anyFieldEdited)
    {
        const auto *tc   = _scene->Get<Assisi::Runtime::Transform>(_selectedEntity);
        const auto *rbc  = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity);
        const auto *desc = _scene->Get<Assisi::Physics::RigidBodyDescriptor>(_selectedEntity);
        if (tc && rbc)
            _physics.SetBodyTransform(*rbc, tc->position, tc->rotation);
        if (rbc && desc)
        {
            _physics.ReshapeBody(*rbc, Assisi::Physics::PhysicsWorld::ColliderShapeDesc{
                                           .shape       = desc->shape,
                                           .halfExtents = desc->halfExtents,
                                           .radius      = desc->radius,
                                           .halfHeight  = desc->halfHeight});
            _physics.SetBodyCCD(*rbc, desc->enableCCD);
        }
    }

    // IsAnyItemActive() is global — it fires for a drag/edit in *any* window, so
    // touching the AA combo or the Save-As field would otherwise freeze the
    // selected body to Static. Scope it to the Inspector: this runs inside the
    // Inspector's Begin/End, so IsWindowFocused (current window) is true only
    // while an Inspector widget is the one being manipulated.
    const bool nowDragging =
        ImGui::IsAnyItemActive() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (nowDragging != _wasDragging)
    {
        const auto *rbc =
            (_selectedEntity != Assisi::ECS::NullEntity && _scene->IsAlive(_selectedEntity))
                ? _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity)
                : nullptr;

        if (rbc)
        {
            if (nowDragging)
            {
                _physics.SetBodyMotionType(*rbc, Assisi::Physics::BodyMotion::Static);
            }
            else
            {
                const auto *desc     = _scene->Get<Assisi::Physics::RigidBodyDescriptor>(_selectedEntity);
                const bool  isStatic = desc && desc->isStatic;
                _physics.SetBodyMotionType(*rbc, isStatic ? Assisi::Physics::BodyMotion::Static
                                                          : Assisi::Physics::BodyMotion::Dynamic);
                if (!isStatic)
                {
                    const auto *tc = _scene->Get<Assisi::Runtime::Transform>(_selectedEntity);
                    if (tc)
                        _physics.SetBodyTransform(*rbc, tc->position, tc->rotation);
                }
            }
        }
    }
    _wasDragging = nowDragging;
}

void SandboxApp::AddComponentToSelected(const Assisi::Core::Reflect::ComponentMeta &meta)
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
    Sandbox::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, meta.id, "Add " + meta.name, _selectedEntity);

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
        // world origin.
        if (auto *tc = _scene->Get<Assisi::Runtime::Transform>(_selectedEntity))
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
            AddPhysicsBody(_selectedEntity, *tc, *desc);
        }
    }

    if (history != nullptr)
        history->CommitGesture(_selectedEntity, meta.id);
}

void SandboxApp::RemoveComponentFromSelected(const Assisi::Core::Reflect::ComponentMeta &meta)
{
    if (_selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity))
    {
        return;
    }

    // Capture the present-before state so undo can restore the removed component.
    Sandbox::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, meta.id, "Remove " + meta.name, _selectedEntity);

    // Tear down runtime state that lives outside the reflected fields before the
    // pool entry disappears. A RigidBodyDescriptor owns a Jolt body (via the
    // transient RigidBody handle); drop both so the collider stops simulating.
    // MeshRenderer's transient pointers are non-owning (the AssetCache owns the
    // GPU resources), so removing it needs no extra cleanup.
    if (meta.name == "RigidBodyDescriptor")
    {
        if (const auto *rbc = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity))
        {
            _physics.RemoveBody(*rbc);
        }
        _scene->Remove<Assisi::Physics::RigidBody>(_selectedEntity);
    }

    _scene->RemoveById(_selectedEntity, meta.id);

    if (history != nullptr)
        history->CommitGesture(_selectedEntity, meta.id);
}

void SandboxApp::DrawInspector()
{
    using namespace Assisi::Core::Reflect;

    ImGui::Begin("Inspector");

    if (_selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity))
    {
        ImGui::TextDisabled("No entity selected.");
        ImGui::TextDisabled("Left-click an object in the scene.");
        ImGui::End();
        return;
    }

    ImGui::Text("Entity [%u:%u]", _selectedEntity.index, _selectedEntity.generation);

    // Rename field: every entity gets an always-available name box. It reads the
    // optional Name component and creates one on first edit, so naming an entity
    // is a single click-and-type — no "add component" step. An empty name leaves
    // the entity showing its id in the list.
    {
        Assisi::Runtime::Name *nameComp = _scene->Get<Assisi::Runtime::Name>(_selectedEntity);

        // Record-before-write for the rename: capture the Name state (present or
        // absent) before the field can change it, so the first-keystroke *creation*
        // of the Name component is captured as absent -> present.
        if (Sandbox::EditHistory *history = ActiveHistory())
            history->RecordBefore(_selectedEntity,
                                  Assisi::Core::Reflect::ComponentIdOf<Assisi::Runtime::Name>(), "Rename",
                                  _selectedEntity);

        char nameBuf[Assisi::Core::kShortStringMax + 1] = {};
        if (nameComp != nullptr)
            nameComp->value.ToCStr(nameBuf, sizeof(nameBuf));
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputTextWithHint("##entityname", "Name", nameBuf, sizeof(nameBuf)))
        {
            if (nameComp == nullptr)
                nameComp = _scene->Add<Assisi::Runtime::Name>(_selectedEntity, {});
            if (nameComp != nullptr)
                nameComp->value.Assign(nameBuf);
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
        if (ImGui::CollapsingHeader(meta->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Record-before-write: snapshot this component's pre-edit JSON before its
            // widgets (which write in-place). Idempotent across a drag; the sweep at
            // end of frame commits or drops it. See EditHistory.hpp §5.
            if (Sandbox::EditHistory *history = ActiveHistory())
                history->RecordBefore(_selectedEntity, meta->id, meta->name, _selectedEntity);

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
            const auto [linearVelocity, angularVelocity] = _physics.GetBodyVelocity(*rbc);
            ImGui::Text("Linear  (m/s):   %.3f, %.3f, %.3f", linearVelocity.x, linearVelocity.y,
                        linearVelocity.z);
            ImGui::Text("Angular (rad/s): %.3f, %.3f, %.3f", angularVelocity.x, angularVelocity.y,
                        angularVelocity.z);
            ImGui::Text("Speed:  %.3f m/s", glm::length(linearVelocity));
            ImGui::Text("CCD:    %s", _physics.IsBodyCCDEnabled(*rbc) ? "LinearCast (on)" : "Discrete (off)");
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
        int  move  = 0;     // -1 = up, +1 = down (Tab or arrows)
        bool reset = false; // text edited -> snap back to the first row
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
                    (_addComponentSelected + nav.move + static_cast<int>(shown)) % static_cast<int>(shown);
            _addComponentSelected = std::clamp(_addComponentSelected, 0, static_cast<int>(shown) - 1);

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
                                          static_cast<int>(i) == _addComponentSelected))
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

    HandlePhysicsEditing(anyFieldEdited);
    ImGui::End();
}
