/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScenePick.hpp
/// @brief What a viewport click hits: the ray, the volumes it is tested against,
/// and the walk that resolves the nearest entity under it.
///
/// Kept out of EditorApp so the pick can be exercised without a window, a device
/// or an ImGui frame — the geometry is where picking goes wrong, and it is pure
/// math.

#include <optional>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{

/// @brief A camera ray through a screen point, plus the camera basis the
/// billboards are oriented by — so what is clickable is exactly what is drawn.
struct PickRay
{
    glm::vec3 origin{0.f};
    glm::vec3 direction{0.f, 0.f, -1.f};
    glm::vec3 cameraRight{1.f, 0.f, 0.f};
    glm::vec3 cameraUp{0.f, 1.f, 0.f};
    /// False for a zero-size framebuffer (minimized), where there is no ray.
    bool valid = false;
};

/// @brief The box a meshed entity is picked with when its own bounds are unusable:
/// ±0.5 on every axis, matching the unit primitives.
inline constexpr Geometry::Aabb kFallbackPickBounds{.min = glm::vec3(-0.5f), .max = glm::vec3(0.5f)};

/// @brief Ray vs. the oriented box @p localBounds carried through @p model.
///
/// The slab test runs in model space — the ray is what gets inverse-transformed,
/// so an arbitrary affine @p model (including non-uniform scale) needs no re-fit.
/// @p tOut is the near hit, or the far one when the ray starts inside.
[[nodiscard]] bool RayAabbIntersect(glm::vec3 origin, glm::vec3 dir, const Geometry::Aabb &localBounds,
                                    const glm::mat4 &model, float &tOut);

/// @brief Ray vs. a camera-facing quad centred at @p center, spanning ±@p half
///        along the unit @p right and @p up axes.
///
/// The same quad IconPass draws, so a mesh-less entity is clickable over its icon
/// and not over a whole cube.
[[nodiscard]] bool RayBillboardIntersect(glm::vec3 origin, glm::vec3 dir, glm::vec3 center, glm::vec3 right,
                                         glm::vec3 up, float half, float &tOut);

/// @brief The local-space box @p bounds should be picked with.
///
/// Mesh bounds as fitted, except for two cases they do not cover: a mesh flat on
/// an axis (a ground quad) gains a small thickness, since a razor-thin slab can
/// only be hit exactly edge-on; and an empty box — no geometry, or bounds never
/// fitted — falls back to kFallbackPickBounds so the entity stays clickable.
[[nodiscard]] Geometry::Aabb PickableBounds(const Geometry::Aabb &bounds);

/// @brief The local-space box @p entity is picked with, or nullopt when it has no
/// mesh and is picked by its icon quad instead.
///
/// A MeshRenderer whose buffer is not resident yet keeps the fallback box: the
/// mesh has no bounds to offer until it streams in, and dropping the entity to its
/// icon mid-stream would move the click target under the cursor.
[[nodiscard]] std::optional<Geometry::Aabb> MeshPickBounds(const ECS::Scene &scene, ECS::Entity entity);

/// @brief How PickEntityInScene asks for an entity's pick volume. MeshPickBounds
/// is what the editor passes; it is a parameter because a MeshBuffer only carries
/// bounds after a GPU upload, which is what would otherwise put this whole walk
/// out of a headless test's reach.
using PickBoundsLookup = std::optional<Geometry::Aabb> (*)(const ECS::Scene &, ECS::Entity);

/// @brief The entity nearest along @p ray, or NullEntity.
///
/// A meshed entity is tested against the box @p boundsOf reports for it; a
/// placement-only one against its icon quad of half-size @p iconHalf. @p tOut is
/// the winning distance, or float max when nothing is hit — the caller weighs it
/// against a non-entity hit and takes whichever is nearer.
[[nodiscard]] ECS::Entity PickEntityInScene(ECS::Scene &scene, const PickRay &ray, float iconHalf,
                                            PickBoundsLookup boundsOf, float &tOut);

} // namespace Assisi::Editor
