/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWorldCells.cpp
/// @brief A WorldMatrix is its world cell and a float matrix measured from that
///        cell's centre: far out it holds no large number, a scene moved by
///        whole cells propagates to the same matrices, and nothing is lost
///        turning it into a matrix measured from anywhere.

#include <doctest/doctest.h>

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/RenderOffset.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Math/WorldCell.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace Assisi;

namespace
{

/// A whole number of cells, about 500 km out on x and as far the other way on
/// z, and a little way up.
constexpr glm::ivec3 kFarCells{977, 1, -977};

/// Positions on the 2^-9 m grid, so each is exact as a float offset in its cell
/// and comparisons can be to the bit; and off the far coarser grid a float has
/// hundreds of kilometres out, so a position that went through one there would
/// not come back.
constexpr glm::dvec3 kNearOrigin{12.501953125, 3.25, -40.123046875};

/// How far past a cell's edge a child is put, from a parent standing just short
/// of it (m).
constexpr double kShortOfEdge = 1.5;
constexpr double kPastEdge = 2.0;

/// The most a translation inside a cell can be on any axis (m).
constexpr float kHalfCell = 0.5f * static_cast<float>(Math::kWorldCellSize);

/// A tree a few levels deep at @p at: a root, turned and scaled, with a child
/// and a grandchild hanging off it, as a level builds one.
std::vector<ECS::Entity> BuildTree(ECS::Scene &scene, const glm::dvec3 &at)
{
    const ECS::Entity root = scene.Create();
    (void)scene.Add(root, ECS::Transform{.position = at,
                                         .rotation = glm::angleAxis(0.5f, glm::normalize(glm::vec3{1.f, 2.f, 3.f})),
                                         .scale = glm::vec3{2.f}});
    const ECS::Entity child = scene.Create();
    (void)scene.Add(child, ECS::Transform{.position = {1.5, -0.25, 3.0}});
    (void)scene.Add(child, ECS::Parent{.parent = root});
    const ECS::Entity grandchild = scene.Create();
    (void)scene.Add(grandchild, ECS::Transform{.position = {0.0, 2.0, 0.5},
                                               .rotation = glm::angleAxis(1.0f, glm::vec3{0.f, 1.f, 0.f})});
    (void)scene.Add(grandchild, ECS::Parent{.parent = child});
    return {root, child, grandchild};
}

const ECS::WorldMatrix &WorldOf(const ECS::Scene &scene, ECS::Entity entity)
{
    return *scene.Get<ECS::WorldMatrix>(entity);
}

} // namespace

TEST_CASE("A root far out is drawn from its own cell, and nothing in its matrix is large")
{
    ECS::Scene scene;
    const glm::dvec3 at = Math::WorldCellCentre(kFarCells) + kNearOrigin;
    const ECS::Entity entity = scene.Create();
    (void)scene.Add(entity, ECS::Transform{.position = at});
    (void)ECS::PropagateTransforms(scene, 0);

    const ECS::WorldMatrix &world = WorldOf(scene, entity);
    CHECK(world.cell == kFarCells);
    CHECK(Math::TranslationOf(world.inCell) == glm::vec3(kNearOrigin));
    CHECK(ECS::WorldPositionOf(world) == at);
}

TEST_CASE("A scene moved by whole cells propagates to the same matrices, a whole number of cells along")
{
    ECS::Scene near;
    ECS::Scene far;
    const std::vector<ECS::Entity> nearTree = BuildTree(near, kNearOrigin);
    const std::vector<ECS::Entity> farTree = BuildTree(far, Math::WorldCellCentre(kFarCells) + kNearOrigin);
    (void)ECS::PropagateTransforms(near, 0);
    (void)ECS::PropagateTransforms(far, 0);

    for (std::size_t i = 0; i < nearTree.size(); ++i)
    {
        CAPTURE(i);
        const ECS::WorldMatrix &nearWorld = WorldOf(near, nearTree[i]);
        const ECS::WorldMatrix &farWorld = WorldOf(far, farTree[i]);
        CHECK(farWorld.inCell == nearWorld.inCell);
        CHECK(farWorld.cell - nearWorld.cell == kFarCells);
    }
}

TEST_CASE("A child that reaches past its parent's cell is drawn from its own")
{
    ECS::Scene scene;
    const double edge = 0.5 * Math::kWorldCellSize;
    const ECS::Entity parent = scene.Create();
    (void)scene.Add(parent, ECS::Transform{.position = {edge - kShortOfEdge, 0.0, 0.0}});
    const ECS::Entity child = scene.Create();
    (void)scene.Add(child, ECS::Transform{.position = {kShortOfEdge + kPastEdge, 0.0, 0.0}});
    (void)scene.Add(child, ECS::Parent{.parent = parent});
    (void)ECS::PropagateTransforms(scene, 0);

    const ECS::WorldMatrix &world = WorldOf(scene, child);
    CHECK(world.cell == glm::ivec3{1, 0, 0});
    CHECK(std::abs(Math::TranslationOf(world.inCell).x) <= kHalfCell);
    CHECK(ECS::WorldPositionOf(world) == glm::dvec3{edge + kPastEdge, 0.0, 0.0});
}

TEST_CASE("A render offset that carries an entity past its cell's edge moves it into the next one")
{
    ECS::Scene scene;
    const double edge = 0.5 * Math::kWorldCellSize;
    const ECS::Entity entity = scene.Create();
    (void)scene.Add(entity, ECS::Transform{.position = {0.0, 0.0, -(edge - kShortOfEdge)}});
    (void)scene.Add(entity, ECS::RenderOffset{.position = {0.f, 0.f, -static_cast<float>(kShortOfEdge + kPastEdge)}});
    (void)ECS::PropagateTransforms(scene, 0);

    const ECS::WorldMatrix &world = WorldOf(scene, entity);
    CHECK(world.cell == glm::ivec3{0, 0, -1});
    CHECK(std::abs(Math::TranslationOf(world.inCell).z) <= kHalfCell);
    CHECK(ECS::WorldPositionOf(world) == glm::dvec3{0.0, 0.0, -(edge + kPastEdge)});
}

TEST_CASE("A matrix measured from its own cell's centre is its in-cell matrix, and back again")
{
    ECS::Scene scene;
    const std::vector<ECS::Entity> tree = BuildTree(scene, Math::WorldCellCentre(kFarCells) + kNearOrigin);
    (void)ECS::PropagateTransforms(scene, 0);

    for (const ECS::Entity entity : tree)
    {
        const ECS::WorldMatrix &world = WorldOf(scene, entity);
        const glm::dvec3 centre = Math::WorldCellCentre(world.cell);
        CHECK(ECS::MatrixFrom(world, centre) == world.inCell);
        CHECK(ECS::WorldMatrixAt(ECS::MatrixFrom(world, centre), centre) == world);
    }
}

TEST_CASE("Measured from a camera beside it, an entity hundreds of kilometres out is in small numbers")
{
    ECS::Scene scene;
    const glm::dvec3 at = Math::WorldCellCentre(kFarCells) + kNearOrigin;
    const ECS::Entity entity = scene.Create();
    (void)scene.Add(entity, ECS::Transform{.position = at});
    (void)ECS::PropagateTransforms(scene, 0);

    const glm::dvec3 camera = at + glm::dvec3{-3.0, 1.5, 6.0};
    CHECK(Math::TranslationOf(ECS::MatrixFrom(WorldOf(scene, entity), camera)) == glm::vec3{3.f, -1.5f, -6.f});
}
