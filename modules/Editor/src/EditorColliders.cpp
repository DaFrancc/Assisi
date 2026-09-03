/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorColliders.cpp
/// @brief Collider wireframes: outline every RigidBodyDescriptor's shape while
/// authoring, so invisible collision geometry can be seen and picked.
///
/// Built here, because the editor knows Physics, and drawn through the renderer's
/// generic overlay-line facility (Render::LinePass), which stays Physics-free.
///
/// The wireframe traces the collider's real edges — a box's 12, a sphere's three
/// great circles — not a filled silhouette. Unselected colliders are light green
/// and depth-tested, so scene geometry occludes them; a selected one draws on top
/// (x-ray) in the selection colours from Runtime/SceneRenderer.hpp, so it is never
/// lost behind a wall and never disagrees with the silhouette around the same
/// object's mesh. Hidden entirely while the game is playing.

#include <Assisi/Editor/EditorApp.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Editor/ColliderPose.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Render/LinePass.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <algorithm>

namespace Assisi::Editor
{

namespace
{
using Assisi::Render::LineVertex;

// Wireframe colours, written straight to the scene target (see outline_edge.frag).
// Only the unselected one is defined here: a selected collider borrows the very
// constants the mesh silhouette uses, so a rigidbody and a plain mesh say
// "selected" and "this is the one being edited" in one vocabulary rather than two
// kept in step by hand.
constexpr glm::vec4 kUnselectedColor{0.40f, 0.95f, 0.45f, 1.0f}; // light green
constexpr glm::vec4 kSelectedColor{Assisi::Runtime::kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{Assisi::Runtime::kActiveSelectionOutline, 1.0f};

/// @brief Append the wireframe for one collider descriptor into @p out.
void AppendColliderWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                             const Assisi::Physics::RigidBodyDescriptor &desc)
{
    using Assisi::Physics::ColliderShape;
    switch (desc.shape)
    {
    case ColliderShape::Sphere:
        AddSphereWireframe(out, model, color, desc.radius);
        break;
    case ColliderShape::Capsule:
        AddCapsuleWireframe(out, model, color, desc.radius, desc.halfHeight);
        break;
    case ColliderShape::Cylinder:
        AddCylinderBody(out, model, color, desc.radius, desc.halfHeight);
        break;
    case ColliderShape::Box:
        AddBoxWireframe(out, model, color, desc.halfExtents);
        break;
    }
}
} // namespace

void EditorApp::SubmitColliderWireframes()
{
    _colliderLinesDepthTested.clear();
    _colliderLinesOnTop.clear();
    _colliderEntities.clear();

    // Editor-only: colliders are hidden while the game is live. Returning early
    // submits nothing and suppresses no billboards — the renderer clears both
    // every frame.
    if (_scene == nullptr || _playState == PlayState::Playing)
    {
        return;
    }

    // Selected entities that turned out to be rigid bodies. They get the orange
    // collider outline below, so the generic selection highlight would draw a
    // second mesh silhouette on top of it; the tail of this function removes them
    // from it.
    std::vector<Assisi::ECS::Entity> outlinedAsBodies;

    for (auto [entity, tc, desc] :
         _scene->Query<Assisi::ECS::Transform, Assisi::Physics::RigidBodyDescriptor>())
    {
        _colliderEntities.push_back(entity);

        const bool selected = IsSelected(entity);
        const bool active   = selected && entity == _selectedEntity;

        // A selected body draws on top in orange, the rest depth-tested green. The
        // active one is redder still, so a multi-selection says which member the
        // inspector and the gizmo are actually addressing.
        const glm::vec4 lineColor = active     ? kActiveSelectedColor
                                    : selected ? kSelectedColor
                                               : kUnselectedColor;

        // The world pose the Jolt body was built at, position and rotation only —
        // see ColliderBodyModel. A parented body lives at its resolved world pose,
        // so tracing its local offset would put the wireframe somewhere the body
        // is not, and disagree with the mesh silhouette drawn below.
        const glm::mat4 bodyModel = ColliderBodyModel(*_scene, entity, tc);

        // The traced edges go out for EVERY collider.
        std::vector<Assisi::Render::LineVertex> &lineOut =
            selected ? _colliderLinesOnTop : _colliderLinesDepthTested;
        AppendColliderWireframe(lineOut, bodyModel, lineColor, desc);

        // Silhouette outlines (collider volume + entity mesh) are a selection
        // highlight, so only the selection gets them: each one costs a full-screen
        // edge-detect pass per frame, which outlining every rigidbody would
        // multiply by the body count.
        if (selected)
        {
            outlinedAsBodies.push_back(entity);
            const glm::vec3 outlineColor = glm::vec3(active ? kActiveSelectedColor : kSelectedColor);
            SubmitColliderOutline(bodyModel, desc, outlineColor);

            // Entity mesh silhouette, if the body has a visible mesh. Full world
            // matrix here, so it hugs the rendered mesh — scale and parenting
            // included.
            if (const Assisi::Runtime::MeshRenderer *mrc = _scene->Get<Assisi::Runtime::MeshRenderer>(entity);
                mrc != nullptr && mrc->meshBuffer != nullptr)
            {
                _sceneRenderer.SubmitOutline(mrc->meshBuffer, tc.worldMatrix, outlineColor);
            }
        }
    }

    _sceneRenderer.SubmitOverlayLines(_colliderLinesDepthTested, /*onTop=*/ false);
    _sceneRenderer.SubmitOverlayLines(_colliderLinesOnTop, /*onTop=*/ true);
    _sceneRenderer.SetIconSuppressedEntities(_colliderEntities);

    // Drop the bodies from the generic selection highlight: their mesh and collider
    // outlines already went out above, and the highlight would draw a second mesh
    // silhouette over them. Per entity rather than all-or-nothing — with a body and
    // a plain mesh both selected, clearing the whole set would leave the mesh with
    // no outline at all.
    if (!outlinedAsBodies.empty())
    {
        std::vector<Assisi::ECS::Entity> stillHighlighted;
        stillHighlighted.reserve(_selection.size());
        for (const Assisi::ECS::Entity entity : _selection)
        {
            if (std::find(outlinedAsBodies.begin(), outlinedAsBodies.end(), entity) == outlinedAsBodies.end())
                stillHighlighted.push_back(entity);
        }
        _sceneRenderer.SetHighlightedEntities(stillHighlighted);
    }
}

void EditorApp::SubmitColliderOutline(const glm::mat4 &bodyModel,
                                      const Assisi::Physics::RigidBodyDescriptor &desc, const glm::vec3 &color)
{
    using Assisi::Physics::ColliderShape;
    using Item = Assisi::Render::OutlinePass::OutlineItem;

    // One group per collider: its own outline pass, so it cannot merge with the
    // entity mesh's outline. Within a group the meshes DO union — a capsule's
    // cylinder body and two end spheres are one silhouette.
    std::vector<Item> items;
    switch (desc.shape)
    {
    case ColliderShape::Sphere:
        items.push_back({&_colliderSphereMesh, glm::scale(bodyModel, glm::vec3(desc.radius))});
        break;
    case ColliderShape::Cylinder:
        items.push_back({&_colliderCylinderMesh, glm::scale(bodyModel, glm::vec3(desc.radius, desc.halfHeight,
                                                                                 desc.radius))});
        break;
    case ColliderShape::Capsule:
        items.push_back({&_colliderCylinderMesh, glm::scale(bodyModel, glm::vec3(desc.radius, desc.halfHeight,
                                                                                 desc.radius))});
        items.push_back({&_colliderSphereMesh, glm::scale(glm::translate(bodyModel,
                                                                         glm::vec3(0.f, desc.halfHeight, 0.f)),
                                                          glm::vec3(desc.radius))});
        items.push_back({&_colliderSphereMesh, glm::scale(glm::translate(bodyModel,
                                                                         glm::vec3(0.f, -desc.halfHeight, 0.f)),
                                                          glm::vec3(desc.radius))});
        break;
    case ColliderShape::Box:
        // Unit cube spans ±0.5, so scale by 2·halfExtents to reach ±halfExtents.
        items.push_back({&_colliderBoxMesh, glm::scale(bodyModel, desc.halfExtents * 2.0f)});
        break;
    }
    _sceneRenderer.SubmitOutlineGroup(items, color);
}

} // namespace Assisi::Editor
