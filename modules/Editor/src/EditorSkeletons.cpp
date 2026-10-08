/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorSkeletons.cpp
/// @brief Skinned meshes' skeletons drawn over the scene, and the joint under the
/// cursor named, so a joint imported in the wrong place or under the wrong parent
/// shows at a glance.
///
/// Drawn on top: a skeleton sits inside its mesh, so depth testing would hide all
/// of it.

#include <Assisi/Editor/EditorApp.hpp>

#include <imgui.h>

#include <limits>
#include <string>

#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Editor/SkeletonOverlay.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include "ImGuiQueries.hpp"

namespace Assisi::Editor
{

namespace
{

constexpr glm::vec4 kBoneColor{0.35f, 0.75f, 1.0f, 1.0f}; // light blue, apart from collider green
constexpr glm::vec4 kSelectedBoneColor{kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedBoneColor{kActiveSelectionOutline, 1.0f};

} // namespace

bool EditorApp::SkeletonsShown() const
{
    return _scene != nullptr && _showEditorOverlays && _showSkeletons && _playState != PlayState::Playing;
}

void EditorApp::SubmitSkeletons()
{
    _skeletonLines.clear();
    if (!SkeletonsShown())
    {
        return;
    }

    for (auto [entity, skinned, renderer, world] :
         _scene->Query<Assisi::Runtime::SkinnedMesh, Assisi::Runtime::MeshRenderer, Assisi::ECS::WorldMatrix>())
    {
        if (skinned.boundMeshId == Assisi::Runtime::kUnboundMesh || renderer.meshBuffer == nullptr)
        {
            continue;
        }
        const bool selected = IsSelected(entity);
        const glm::vec4 color = !selected                  ? kBoneColor
                                : entity == _selectedEntity ? kActiveSelectedBoneColor
                                                            : kSelectedBoneColor;
        AppendSkeletonLines(_skeletonLines, renderer.meshBuffer->Skeleton(), skinned.jointModel, world.matrix, color);
    }
    _overlays.SubmitOverlayLines(_skeletonLines, /*onTop=*/ true);
}

void EditorApp::DrawSkeletonJointTooltip()
{
    if (!SkeletonsShown() || ImGuiWantsMouse())
    {
        return;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const glm::vec2 cursor{mouse.x, mouse.y};
    const PickRay ray = BuildPickRay(cursor);
    if (!ray.valid)
    {
        return;
    }

    const std::string *name = nullptr;
    float nearestPixels = std::numeric_limits<float>::max();
    for (auto [entity, skinned, renderer, world] :
         _scene->Query<Assisi::Runtime::SkinnedMesh, Assisi::Runtime::MeshRenderer, Assisi::ECS::WorldMatrix>())
    {
        if (skinned.boundMeshId == Assisi::Runtime::kUnboundMesh || renderer.meshBuffer == nullptr)
        {
            continue;
        }
        float pixels = 0.f;
        const int32_t joint = JointUnderCursor(ray, cursor, skinned.jointModel, world.matrix, pixels);
        if (joint != Assisi::Geometry::kNoJoint && pixels < nearestPixels)
        {
            nearestPixels = pixels;
            name = &renderer.meshBuffer->Skeleton().Names[static_cast<std::size_t>(joint)];
        }
    }
    if (name != nullptr)
    {
        ImGui::SetTooltip("%s", name->c_str());
    }
}

} // namespace Assisi::Editor
