/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <string>

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

        void *fp = static_cast<char *>(mut) + field.offset;
        ImGui::PushID(field.name.c_str());

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
        default:
            ImGui::TextDisabled("%s: [unsupported type]", field.name.c_str());
            break;
        }

        anyFieldEdited |= edited;
        ImGui::PopID();
    }

    if (!anyEditable)
        ImGui::TextDisabled("(runtime-only)");

    return anyFieldEdited;
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
            _physics.ReshapeBox(*rbc, desc->halfExtents);
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
    ImGui::Separator();

    bool anyFieldEdited = false;

    // SerializableComponents() skips ACOMP(transient) id-only components
    // (e.g. RigidBody/DestroyTag), which have no getByEntity hook and nothing
    // to edit, so no per-item guard is needed here.
    for (const auto *meta : ComponentRegistry::Instance().SerializableComponents())
    {
        const void *compPtr =
            meta->getByEntity(_scene, _selectedEntity.index, _selectedEntity.generation);

        if (!compPtr)
            continue;

        if (!ImGui::CollapsingHeader(meta->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            continue;

        ImGui::PushID(meta->name.c_str());
        const bool edited = EditComponentFields(const_cast<void *>(compPtr), *meta);
        ImGui::PopID();

        // Field edits write component memory by offset, bypassing Scene::GetMut's
        // change stamping, so report it explicitly. No-op for untracked components;
        // for a tracked one (Transform) this is what re-propagates the edit.
        if (edited)
            _scene->MarkChanged(_selectedEntity, meta->id);
        anyFieldEdited |= edited;
    }

    // A typed AssetPath edit changes meshPath/materialOverrides; re-resolve so the
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

    HandlePhysicsEditing(anyFieldEdited);
    ImGui::End();
}
