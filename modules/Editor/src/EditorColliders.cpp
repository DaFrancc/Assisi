/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorColliders.cpp
/// @brief Editor collider wireframes: outline every RigidBodyDescriptor's shape in
/// the world while authoring, so invisible collision geometry is visible and
/// selectable. Built here (the editor knows Physics) and drawn through the renderer's
/// generic overlay-line facility (Render::LinePass), which stays Physics-free.
///
/// The wireframe traces the collider's actual edges (a box's 12 edges, a sphere's
/// three great circles, ...), not a filled silhouette. Unselected colliders draw
/// light green and depth-tested (occluded by scene geometry, so they recede);
/// the selected collider draws yellow-orange and on-top (x-ray), so it is never
/// lost behind a wall. Hidden entirely while the game is playing.

#include <Assisi/Editor/EditorApp.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Render/LinePass.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::Editor
{

namespace
{
using Assisi::Render::LineVertex;

// Wireframe colours (written straight to the scene target, like the selection
// outline's orange — see outline_edge.frag). Selected is a distinct yellow-orange
// so it never reads as the red-orange selection silhouette drawn around the mesh.
constexpr glm::vec4 kUnselectedColor{0.40f, 0.95f, 0.45f, 1.0f}; // light green
constexpr glm::vec4 kSelectedColor{1.00f, 0.65f, 0.10f, 1.0f};   // yellow-orange

// Tessellation of curved shapes. 24 segments per full circle is smooth enough for
// an editor overlay without flooding the line batch.
constexpr int32_t kCircleSegments = 24;

const glm::vec3 kAxisX{1.f, 0.f, 0.f};
const glm::vec3 kAxisY{0.f, 1.f, 0.f};
const glm::vec3 kAxisZ{0.f, 0.f, 1.f};

/// @brief Append one segment, transforming both local endpoints by @p model.
void AddSegment(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, const glm::vec3 &a,
                const glm::vec3 &b)
{
    out.push_back({glm::vec3(model * glm::vec4(a, 1.f)), color});
    out.push_back({glm::vec3(model * glm::vec4(b, 1.f)), color});
}

/// @brief Append a poly-line arc of @p segments in the plane spanned by unit axes
/// @p u, @p v, centred at @p center with radius @p radius, sweeping angle
/// [@p a0, @p a1]. A full circle is a0=0, a1=2π.
void AddArc(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, const glm::vec3 &center,
            const glm::vec3 &u, const glm::vec3 &v, float radius, float a0, float a1, int32_t segments)
{
    glm::vec3 prev = center + radius * (std::cos(a0) * u + std::sin(a0) * v);
    for (int32_t i = 1; i <= segments; ++i)
    {
        const float t   = a0 + (a1 - a0) * (static_cast<float>(i) / static_cast<float>(segments));
        glm::vec3   cur = center + radius * (std::cos(t) * u + std::sin(t) * v);
        AddSegment(out, model, color, prev, cur);
        prev = cur;
    }
}

void AddBoxWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                     const glm::vec3 &halfExtents)
{
    const glm::vec3 &h = halfExtents;
    // Eight corners, indexed by sign bits (x = bit0, y = bit1, z = bit2).
    glm::vec3 c[8];
    for (int32_t i = 0; i < 8; ++i)
    {
        c[i] = {(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
    }
    // 12 edges: pairs of corners differing in exactly one axis bit.
    constexpr int32_t edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},  // along X
                                  {0, 2}, {1, 3}, {4, 6}, {5, 7},  // along Y
                                  {0, 4}, {1, 5}, {2, 6}, {3, 7}}; // along Z
    for (const auto &e : edges)
    {
        AddSegment(out, model, color, c[e[0]], c[e[1]]);
    }
}

void AddSphereWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, float radius)
{
    const float full = glm::two_pi<float>();
    AddArc(out, model, color, glm::vec3(0.f), kAxisX, kAxisY, radius, 0.f, full, kCircleSegments);
    AddArc(out, model, color, glm::vec3(0.f), kAxisX, kAxisZ, radius, 0.f, full, kCircleSegments);
    AddArc(out, model, color, glm::vec3(0.f), kAxisY, kAxisZ, radius, 0.f, full, kCircleSegments);
}

/// @brief Two rings (at ±halfHeight along Y) plus four vertical connectors — the
/// shared cylindrical body of a cylinder and a capsule.
void AddCylinderBody(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, float radius,
                     float halfHeight)
{
    const glm::vec3 top(0.f, halfHeight, 0.f);
    const glm::vec3 bot(0.f, -halfHeight, 0.f);
    const float     full = glm::two_pi<float>();
    AddArc(out, model, color, top, kAxisX, kAxisZ, radius, 0.f, full, kCircleSegments);
    AddArc(out, model, color, bot, kAxisX, kAxisZ, radius, 0.f, full, kCircleSegments);
    for (int32_t k = 0; k < 4; ++k)
    {
        const float     angle = static_cast<float>(k) * glm::half_pi<float>();
        const glm::vec3 offset = radius * (std::cos(angle) * kAxisX + std::sin(angle) * kAxisZ);
        AddSegment(out, model, color, top + offset, bot + offset);
    }
}

void AddCapsuleWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color, float radius,
                         float halfHeight)
{
    AddCylinderBody(out, model, color, radius, halfHeight);

    const glm::vec3 top(0.f, halfHeight, 0.f);
    const glm::vec3 bot(0.f, -halfHeight, 0.f);
    const float     pi = glm::pi<float>();
    const int32_t   capSegs = kCircleSegments / 2;
    // Two orthogonal profile half-arcs per hemisphere: from one rim point, over the
    // pole, to the opposite rim point. Top domes up (+Y), bottom domes down (−Y).
    AddArc(out, model, color, top, kAxisX, kAxisY, radius, 0.f, pi, capSegs);
    AddArc(out, model, color, top, kAxisZ, kAxisY, radius, 0.f, pi, capSegs);
    AddArc(out, model, color, bot, kAxisX, kAxisY, radius, 0.f, -pi, capSegs);
    AddArc(out, model, color, bot, kAxisZ, kAxisY, radius, 0.f, -pi, capSegs);
}

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

    // Editor-only: colliders are hidden while the game is live. Nothing submitted,
    // no billboards suppressed (the renderer clears both each frame anyway).
    if (_scene == nullptr || _playState == PlayState::Playing)
    {
        return;
    }

    bool selectedIsRigidBody = false;

    for (auto [entity, tc, desc] :
         _scene->Query<Assisi::ECS::Transform, Assisi::Physics::RigidBodyDescriptor>())
    {
        _colliderEntities.push_back(entity);

        const bool selected = (entity == _selectedEntity);

        // Selected body draws on top in orange; the rest are depth-tested green.
        const glm::vec4 lineColor = selected ? kSelectedColor : kUnselectedColor;

        // The Jolt body is placed from the Transform's local position+rotation only
        // (no scale — see AddPhysicsBody), and the descriptor's dimensions are
        // absolute world units, so the wireframe/outline match the body exactly.
        glm::mat4 bodyModel = glm::mat4_cast(tc.rotation);
        bodyModel[3]        = glm::vec4(tc.position, 1.f);

        // Traced collider edges (line wireframe) — drawn for EVERY collider.
        std::vector<Assisi::Render::LineVertex> &lineOut =
            selected ? _colliderLinesOnTop : _colliderLinesDepthTested;
        AppendColliderWireframe(lineOut, bodyModel, lineColor, desc);

        // Silhouette outlines (collider volume + entity mesh) are a selection
        // highlight — drawn only for the selected object. Outlining every rigidbody
        // meant one full-screen edge-detect pass per object per frame, which is what
        // other engines avoid (they wireframe colliders and outline only the
        // selection). The outline is orange and on top, like a selection highlight.
        if (selected)
        {
            selectedIsRigidBody = true;
            const glm::vec3 outlineColor = glm::vec3(kSelectedColor);
            SubmitColliderOutline(bodyModel, desc, outlineColor);

            // Entity mesh silhouette (if the body has a visible mesh). Uses the full
            // world matrix so it hugs the rendered mesh, scale/parenting included.
            if (const Assisi::Runtime::MeshRenderer *mrc = _scene->Get<Assisi::Runtime::MeshRenderer>(entity);
                mrc != nullptr && mrc->meshBuffer != nullptr)
            {
                _sceneRenderer.SubmitOutline(mrc->meshBuffer, tc.worldMatrix, outlineColor);
            }
        }
    }

    _sceneRenderer.SubmitOverlayLines(_colliderLinesDepthTested, /*onTop=*/false);
    _sceneRenderer.SubmitOverlayLines(_colliderLinesOnTop, /*onTop=*/true);
    _sceneRenderer.SetIconSuppressedEntities(_colliderEntities);

    // A selected rigidbody's mesh + collider outline already come from the orange
    // on-top submissions above, so drop the generic selection highlight to avoid a
    // redundant second mesh outline; a selected non-rigidbody still uses it.
    if (selectedIsRigidBody)
    {
        _sceneRenderer.SetHighlightedEntity(Assisi::ECS::NullEntity);
    }
}

void EditorApp::SubmitColliderOutline(const glm::mat4 &bodyModel,
                                       const Assisi::Physics::RigidBodyDescriptor &desc, const glm::vec3 &color)
{
    using Assisi::Physics::ColliderShape;
    using Item = Assisi::Render::OutlinePass::OutlineItem;

    // One group per collider — its own outline pass, independent of the entity mesh
    // outline (they must not merge). A capsule needs several meshes that DO union
    // together (cylinder body + two end spheres) to form its single silhouette.
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
