/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorColliders.cpp
/// @brief Collider wireframes: outline every Collider's shape while
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
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Editor/ColliderPose.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Editor/Overlay/LinePass.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <algorithm>

namespace Assisi::Editor
{

namespace
{
using Assisi::Editor::LineVertex;

// Wireframe colours, written straight to the scene target (see outline_edge.frag).
// Only the unselected one is defined here: a selected collider borrows the very
// constants the mesh silhouette uses, so a rigidbody and a plain mesh say
// "selected" and "this is the one being edited" in one vocabulary rather than two
// kept in step by hand.
constexpr glm::vec4 kUnselectedColor{0.40f, 0.95f, 0.45f, 1.0f}; // light green
constexpr glm::vec4 kSelectedColor{Assisi::Editor::kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{Assisi::Editor::kActiveSelectionOutline, 1.0f};

/// @brief Append the wireframe for one collider into @p out. A model's shape
/// is traced from @p modelEdges, pairs of points in the collider's own space.
void AppendColliderWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                             const Assisi::Physics::Collider &desc, const std::vector<glm::vec3> &modelEdges)
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
    case ColliderShape::Convex:
    case ColliderShape::Mesh:
        for (std::size_t i = 0; i + 1 < modelEdges.size(); i += 2)
        {
            AddSegment(out, model, color, modelEdges[i], modelEdges[i + 1]);
        }
        break;
    case ColliderShape::Count_:
        break;
    }
}
/// @brief Where @p collider's shape sits, given its body's world pose
/// @p bodyModel, the scale the shape is built at, and the entity's world
/// @p entityScale, which moves the offset as the physics world does.
glm::mat4 ColliderShapeModel(const glm::mat4 &bodyModel, const Assisi::Physics::Collider &collider,
                             const glm::vec3 &shapeScale, const glm::vec3 &entityScale)
{
    const glm::mat4 offset = glm::translate(bodyModel, collider.offsetPosition * entityScale) *
                             glm::mat4_cast(glm::normalize(collider.offsetRotation));
    return glm::scale(offset, shapeScale);
}

/// @brief The length of each axis of @p world: the entity's scale composed
/// through its parents.
glm::vec3 WorldScaleOf(const glm::mat4 &world)
{
    using Assisi::Math::ColumnOf;
    using Assisi::Math::MatrixColumn;
    return glm::vec3(glm::length(ColumnOf(world, MatrixColumn::Right)), glm::length(ColumnOf(world, MatrixColumn::Up)),
                     glm::length(ColumnOf(world, MatrixColumn::Back)));
}

/// Half the width of the cross drawn at a joint's anchor (m).
constexpr float kJointMarkerHalf = 0.08f;

/// How long a joint's axis is drawn (m).
constexpr float kJointAxisLength = 0.4f;

/// One joint as the overlay draws it, whichever kind it is.
struct JointView
{
    glm::vec3 anchor{0.f};
    glm::vec3 axis{0.f};        ///< Zero for a kind with no axis.
    glm::vec3 otherAnchor{0.f}; ///< A DistanceJoint's far end.
    Assisi::ECS::Entity owner{Assisi::ECS::NullEntity};
    Assisi::ECS::Entity other{Assisi::ECS::NullEntity};
    bool hasOtherAnchor = false;
};

/// Every @p T joint in @p scene, into @p out.
template <typename T> void CollectJoints(const Assisi::ECS::Scene &scene, std::vector<JointView> &out)
{
    for (auto [entity, joint] : scene.Query<T>())
    {
        JointView view;
        view.anchor = joint.anchor;
        view.owner = entity;
        view.other = joint.other;
        if constexpr (requires { joint.axis; })
        {
            view.axis = joint.axis;
        }
        if constexpr (requires { joint.otherAnchor; })
        {
            view.otherAnchor = joint.otherAnchor;
            view.hasOtherAnchor = true;
        }
        out.push_back(view);
    }
}

/// A joint's lines: a cross at its anchor, its axis, and a line to where it
/// holds the other body.
void AppendJointLines(std::vector<LineVertex> &out, const Assisi::ECS::Scene &scene, const JointView &joint,
                      const glm::vec4 &color)
{
    const Assisi::ECS::Transform *transform = scene.Get<Assisi::ECS::Transform>(joint.owner);
    const Assisi::ECS::WorldMatrix *world = scene.Get<Assisi::ECS::WorldMatrix>(joint.owner);
    if (transform == nullptr || world == nullptr)
    {
        return;
    }
    // Anchors are scaled with the entity, as the physics world builds them.
    const glm::mat4 body = glm::scale(ColliderBodyModel(scene, joint.owner, *transform), WorldScaleOf(world->matrix));
    const glm::vec3 anchor = glm::vec3(body * glm::vec4(joint.anchor, 1.f));
    const glm::mat4 identity{1.f};
    for (const glm::vec3 &direction : {kAxisX, kAxisY, kAxisZ})
    {
        AddSegment(out, identity, color, anchor - direction * kJointMarkerHalf, anchor + direction * kJointMarkerHalf);
    }
    if (joint.axis != glm::vec3(0.f))
    {
        const glm::vec3 axis = glm::normalize(glm::vec3(body * glm::vec4(joint.axis, 0.f)));
        AddSegment(out, identity, color, anchor, anchor + axis * kJointAxisLength);
    }

    if (joint.hasOtherAnchor)
    {
        AddSegment(out, identity, color, anchor, glm::vec3(body * glm::vec4(joint.otherAnchor, 1.f)));
        return;
    }
    if (joint.other == Assisi::ECS::NullEntity || !scene.IsAlive(joint.other))
    {
        return;
    }
    if (const Assisi::ECS::WorldMatrix *other = scene.Get<Assisi::ECS::WorldMatrix>(joint.other); other != nullptr)
    {
        AddSegment(out, identity, color, anchor, glm::vec3(other->matrix[3]));
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
         _scene->Query<Assisi::ECS::Transform, Assisi::Physics::Collider>())
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

        // The world pose the Jolt body was built at — see ColliderBodyModel — and
        // the scale its shape was built at, which for a round shape is not the
        // Transform's. A parented body lives at its resolved world pose, so
        // tracing its local offset would put the wireframe somewhere the body is
        // not, and disagree with the mesh silhouette drawn below.
        const glm::mat4 bodyModel =
            ColliderShapeModel(ColliderBodyModel(*_scene, entity, tc), desc, _physics->GetColliderScale(entity),
                               WorldScaleOf(_scene->Get<Assisi::ECS::WorldMatrix>(entity)->matrix));

        // The traced edges go out for EVERY collider.
        std::vector<LineVertex> &lineOut =
            selected ? _colliderLinesOnTop : _colliderLinesDepthTested;
        _physics->CollisionAssetEdges(desc, _colliderModelEdges);
        AppendColliderWireframe(lineOut, bodyModel, lineColor, desc, _colliderModelEdges);

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
            const Assisi::Runtime::MeshRenderer *mrc = _scene->Get<Assisi::Runtime::MeshRenderer>(entity);
            const Assisi::Render::MeshBuffer *mesh =
                mrc != nullptr ? Assisi::Runtime::DrawnMesh(*mrc, _scene->Get<Assisi::Runtime::SkinnedMesh>(entity))
                               : nullptr;
            if (mesh != nullptr)
            {
                const glm::mat4 &world = _scene->Get<Assisi::ECS::WorldMatrix>(entity)->matrix;
                _overlays.SubmitOutline(mesh, world, outlineColor);
            }
        }
    }

    // Characters, on the same lines and in the same colours. Their capsule stands
    // on the entity's Transform rather than being centred on it, so the wireframe
    // is lifted by its own half-height — drawn where the author placed the feet,
    // which is the whole reason a character is authored that way.
    for (auto [entity, tc, desc] :
         _scene->Query<Assisi::ECS::Transform, Assisi::Physics::Character>())
    {
        _colliderEntities.push_back(entity);

        const bool selected = IsSelected(entity);
        const bool active   = selected && entity == _selectedEntity;

        const glm::vec4 lineColor = active     ? kActiveSelectedColor
                                    : selected ? kSelectedColor
                                               : kUnselectedColor;

        const glm::mat4 feet = ColliderBodyModel(*_scene, entity, tc);
        const glm::mat4 bodyModel =
            glm::translate(feet, glm::vec3(0.f, desc.halfHeight + desc.radius, 0.f));

        std::vector<LineVertex> &lineOut =
            selected ? _colliderLinesOnTop : _colliderLinesDepthTested;

        // Only the standing shape. A crouch is a runtime state with no authored
        // pose to draw it at, and two capsules at once would read as two
        // characters.
        AddCapsuleWireframe(lineOut, bodyModel, lineColor, desc.radius, desc.halfHeight);
    }

    // Joints, in the same colours as the body that carries them. A scene with
    // none allocates nothing here.
    std::vector<JointView> joints;
    CollectJoints<Assisi::Physics::FixedJoint>(*_scene, joints);
    CollectJoints<Assisi::Physics::PointJoint>(*_scene, joints);
    CollectJoints<Assisi::Physics::HingeJoint>(*_scene, joints);
    CollectJoints<Assisi::Physics::SliderJoint>(*_scene, joints);
    CollectJoints<Assisi::Physics::DistanceJoint>(*_scene, joints);
    CollectJoints<Assisi::Physics::SwingTwistJoint>(*_scene, joints);
    for (const JointView &joint : joints)
    {
        const bool selected = IsSelected(joint.owner);
        const bool active = selected && joint.owner == _selectedEntity;
        const glm::vec4 lineColor = active     ? kActiveSelectedColor
                                    : selected ? kSelectedColor
                                               : kUnselectedColor;
        AppendJointLines(selected ? _colliderLinesOnTop : _colliderLinesDepthTested, *_scene, joint, lineColor);
    }

    _overlays.SubmitOverlayLines(_colliderLinesDepthTested, /*onTop=*/ false);
    _overlays.SubmitOverlayLines(_colliderLinesOnTop, /*onTop=*/ true);
    _overlays.SetIconSuppressedEntities(_colliderEntities);

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
        _overlays.SetHighlightedEntities(stillHighlighted);
    }
}

void EditorApp::SubmitColliderOutline(const glm::mat4 &bodyModel,
                                      const Assisi::Physics::Collider &desc, const glm::vec3 &color)
{
    using Assisi::Physics::ColliderShape;
    using Item = Assisi::Editor::OutlinePass::OutlineItem;

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
    case ColliderShape::Convex:
    case ColliderShape::Mesh:
    case ColliderShape::Count_:
        // No unit mesh stands for a model's shape; its edges, drawn on top in
        // the selection colour, are its highlight.
        return;
    }
    _overlays.SubmitOutlineGroup(items, color);
}

} // namespace Assisi::Editor
