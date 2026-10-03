/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Transform.hpp
/// @brief Local-space TRS component, and the world matrices its pool keeps
///        beside it.
///
/// Transform is the engine's most foundational component: rendering, physics,
/// and the scene-graph hierarchy all read and write it. It lives here, in the
/// ECS layer, precisely so it stays free of any renderer dependency — lower
/// modules (e.g. Physics) can use it without pulling in nvrhi/GPU headers.
/// Runtime re-exports it from Components.hpp, so `Runtime::Transform` names this
/// exact type.

#include <Assisi/Prelude.hpp>
#include <Assisi/ECS/SparseSet.hpp>
#include <Assisi/Math/GLM.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::ECS
{

/// @brief Local-space TRS.
///
/// Write to position/rotation/scale to move an entity — through Scene::GetMut (or
/// Scene::MarkChanged for a by-offset writer), since Transform is ACOMP(tracked):
/// PropagateTransforms uses that change signal to skip entities whose local TRS
/// and ancestors are unchanged. The world matrix is not a field: the pool keeps
/// it in a lane of its own, filled by PropagateTransforms and read with
/// ECS::WorldMatrix.
///
/// `replicable` as well: pose is the one thing every mirrored entity needs.
/// `tracked` is spelled out beside it rather than left to `replicable`'s
/// implication, so that dropping `replicable` would not silently take
/// PropagateTransforms's change signal with it.
ACOMP(replicable, tracked)
struct Transform
{
    AFIELD() glm::vec3 position{0.f, 0.f, 0.f};
    AFIELD() glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    AFIELD() glm::vec3 scale{1.f, 1.f, 1.f};
};

/// @brief The Transform pool's world matrices, one per Transform in dense
/// order, so a pass over the pool reads them packed.
template <> struct SparseSetLanes<Transform>
{
    std::vector<glm::mat4> world;

    void Push() { world.emplace_back(1.f); }
    void Move(uint32_t to, uint32_t from) { world[to] = world[from]; }
    void Pop() { world.pop_back(); }
    void Clear() { world.clear(); }
};

} // namespace Assisi::ECS
