/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldMatrix.hpp
/// @brief The world-space matrix an entity is drawn with.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Math/WorldCell.hpp>

namespace Assisi::ECS
{

/// @brief Where an entity is drawn, in world space: the world cell it is in,
/// and a float matrix measured from that cell's centre.
///
/// Measured from the cell rather than from the world's origin, so no float in
/// it holds a large number however far out the entity is, and it changes only
/// when the entity moves on screen, never when the camera does. Turn it into a
/// matrix measured from anywhere with MatrixFrom, or read where it is with
/// WorldPositionOf.
///
/// Every Transform brings one. PropagateTransforms writes it: the local pose
/// composed with the parents', blended between fixed steps, with any
/// RenderOffset applied. Read it to draw; read the Transform to simulate, since
/// between steps the two differ. Writing it does nothing lasting — the next
/// propagation that touches the entity overwrites it.
///
/// Tracked: a propagation that moves it on screen stamps its change tick, so
/// ChangedSince<WorldMatrix> is everything whose drawn pose changed — a body
/// blending between two fixed steps included, though its Transform is written
/// only on step frames.
///
/// Identity until the first propagation after it is added. Removing only the
/// Transform leaves it until that propagation, which removes it too.
ACOMP(transient, tracked)
struct WorldMatrix
{
    /// Measured from the centre of @ref cell; its translation is within half a
    /// cell of it on each axis.
    glm::mat4 inCell{1.f};
    glm::ivec3 cell{0};

    bool operator==(const WorldMatrix &) const = default;
};

/// @brief Where @p world is, exactly.
[[nodiscard]] inline glm::dvec3 WorldPositionOf(const WorldMatrix &world)
{
    return Math::WorldCellCentre(world.cell) + glm::dvec3(glm::vec3(world.inCell[3]));
}

/// @brief @p world as a float matrix measured from @p origin. Exact to a float
/// of the distance between them, so small near @p origin wherever both are.
[[nodiscard]] inline glm::mat4 MatrixFrom(const WorldMatrix &world, const glm::dvec3 &origin)
{
    glm::mat4 matrix = world.inCell;
    const glm::dvec3 translation =
        (Math::WorldCellCentre(world.cell) - origin) + glm::dvec3(glm::vec3(world.inCell[3]));
    matrix[3] = glm::vec4(glm::vec3(translation), 1.f);
    return matrix;
}

/// @brief @p matrix, measured from @p origin, as a WorldMatrix: the reverse of
/// MatrixFrom.
[[nodiscard]] inline WorldMatrix WorldMatrixAt(const glm::mat4 &matrix, const glm::dvec3 &origin)
{
    const glm::dvec3 position = origin + glm::dvec3(glm::vec3(matrix[3]));
    WorldMatrix world{.inCell = matrix, .cell = Math::WorldCellOf(position)};
    world.inCell[3] = glm::vec4(glm::vec3(position - Math::WorldCellCentre(world.cell)), 1.f);
    return world;
}

} // namespace Assisi::ECS
