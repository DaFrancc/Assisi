/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Hierarchy.hpp
/// @brief Parent-child entity relationships and world-space transform propagation.
///
/// Hierarchy is opt-in: add Parent to a child entity to attach it to a
/// parent. The parent needs no modification.
///
/// Transform stores local-space TRS. PropagateTransforms() walks the
/// parent chain and writes each entity's world matrix (see WorldMatrix).
/// For root entities (no parent), the world matrix is the local TRS matrix.

#include <cstdint>
#include <vector>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Prelude.hpp>

namespace Assisi::ECS
{

/// @brief Marks an entity as a child of another entity.
///
/// The parent entity must have a Transform. Entities without this
/// component are treated as roots (world matrix == local TRS matrix).
///
/// ACOMP(tracked): PropagateTransforms's dirty-skip must recompute a child when its
/// *parent link* changes (attach or reparent), not only when its Transform changes.
/// Tracking Parent lets Add/GetMut stamp a change tick the propagation can observe;
/// mutate the link through GetMut<Parent> (not the non-stamping Get) for it to take.
///
/// Tracked but **not** replicable, deliberately. Replicating a parent link means
/// replicating the hierarchy itself — ordering guarantees so a child never
/// arrives before its parent, cycle rejection on hostile input, and a decision
/// about whether a mirrored child's pose is its own or its parent's. That is a
/// project, not a flag, and marking this type would quietly promise all of it.
ACOMP(tracked)
struct Parent
{
    AFIELD() ECS::Entity parent = NullEntity;
};

/// @brief Refresh the world matrices of entities whose drawn pose changed.
///
/// Writes results into the Transform pool's world lane (see WorldMatrix). Must be
/// called once per frame before DrawScene() or anything that reads a world
/// matrix. Visits only what can have changed: Transforms and Parent links written
/// since `lastTick`, Parent links removed since then, entities blending between
/// two fixed steps (when the blend alpha moved), entities whose blend just ended,
/// entities with a RenderOffset, and the children of anything recomputed. A
/// static scene costs a scan of two change-tick arrays.
///
/// A Transform written inside a FixedStepScope is drawn between its pose before
/// that step and its current one, at the alpha SetBlendAlpha set.
///
/// @param lastTick The scene change tick this system last ran at (pass 0 on the
///        first call, which recomputes everything).
/// @return The scene's current change tick — pass it back as `lastTick` next frame
///         to skip unchanged entities. Discarding it is safe (you simply lose the
///         skip, recomputing everything if you keep passing 0).
uint64_t PropagateTransforms(Scene &scene, uint64_t lastTick);

/// @brief How many entities the last PropagateTransforms on @p scene recomputed.
[[nodiscard]] uint32_t LastPropagationResolved(const Scene &scene);

/// @brief Marks one fixed step on @p scene for as long as it lives.
///
/// The first write to a Transform while one exists records the pose before it,
/// and the entity is drawn blended from that pose until the next step. A scope
/// rather than begin and end calls, so a step cannot be left open.
class FixedStepScope
{
public:
    explicit FixedStepScope(Scene &scene);
    ~FixedStepScope();

    FixedStepScope(const FixedStepScope &) = delete;
    FixedStepScope &operator=(const FixedStepScope &) = delete;

private:
    Scene &_scene;
};

/// @brief How far drawn poses are from the previous fixed step's to the
/// latest one's, in [0, 1]. Set once a frame, before propagation.
void SetBlendAlpha(Scene &scene, float alpha);

/// @brief Ends every blend, so the next propagation draws each entity at its
/// current pose. For a world that is not stepping, whose alpha still moves.
void SettleTransforms(Scene &scene);

/// @brief Draws @p entity at its current pose, with no blend from where it
/// was. Call after writing the Transform, for a teleport.
void SnapTransform(Scene &scene, Entity entity);

/// @brief Collects @p root plus every entity whose Parent chain leads to it.
///
/// Root-first, breadth-first over the Parent pool. There is no child index, so
/// this scans the scene once per collected entity — fine at the scales this is
/// used (deleting or migrating a subtree). Used by entity migration
/// (App::MigrateEntity) and mirrors the editor's delete-subtree gather.
std::vector<Entity> GatherSubtree(Scene &scene, Entity root);

/// @brief @p entity's pose in world space, resolved by walking its Parent chain.
///
/// Composes local TRS up the chain rather than reading the world matrix,
/// so it answers without a propagation pass having run and returns a TRS the
/// compose/inverse-compose pair can take. Callers that need world poses every
/// frame want PropagateTransforms and the cached matrix instead; this is for the
/// one-shot questions — chiefly "where does this actually stand", asked when a
/// selection is written to a blueprint and its parent is not coming with it.
///
/// Exact under the same uniform-scale rule ComposeTransform states: a
/// non-uniformly scaled ancestor cannot be expressed as one TRS, and the result
/// is the closest one. An entity with no Transform is the identity, and a cycle
/// in Parent terminates the walk rather than hanging on a corrupt scene.
[[nodiscard]] Transform WorldTransformOf(const Scene &scene, Entity entity);

/// @brief @p entity's world matrix as the last PropagateTransforms left it, or
/// null when it has no Transform.
[[nodiscard]] const glm::mat4 *WorldMatrix(const Scene &scene, Entity entity);

/// @brief The world matrix @p entity's Transform is relative to: its parent's
/// propagated world matrix, or null when it has no Parent, its Parent names no
/// entity, or that entity has no Transform.
///
/// Reads the cached matrix, so it is only as current as the last
/// PropagateTransforms. A null answer means @p entity's local pose is its world
/// pose.
[[nodiscard]] const glm::mat4 *ParentWorldMatrix(const Scene &scene, Entity entity);

} // namespace Assisi::ECS
