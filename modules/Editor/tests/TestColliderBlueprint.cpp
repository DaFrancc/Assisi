/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestColliderBlueprint.cpp
/// @brief A model's pieces broken into the children of a collider blueprint:
/// primitives exactly, hulls by reference to the model, each where the model
/// put it.

#include <doctest/doctest.h>

#include <Assisi/Editor/ColliderBlueprint.hpp>

#include <vector>

using namespace Assisi;

namespace
{

Core::AssetId ModelId()
{
    Core::AssetId id;
    id.bytes[0] = 0xB1;
    id.bytes[15] = 1;
    return id;
}

/// A chair: a hull seat, then a capsule leg turned on its side.
Geometry::CollisionData Chair()
{
    Geometry::CollisionData collision;
    collision.points = {{-1.f, 0.f, -1.f}, {1.f, 0.f, -1.f}, {0.f, 0.f, 1.f}, {0.f, 0.5f, 0.f}};

    Geometry::CollisionPiece seat;
    seat.name = "UCX_Seat";
    seat.kind = Geometry::CollisionPieceKind::Hull;
    seat.position = glm::vec3(0.f, 1.f, 0.f);
    seat.firstPoint = 0;
    seat.pointCount = 4;
    collision.pieces.push_back(seat);

    Geometry::CollisionPiece leg;
    leg.name = "UCP_Leg";
    leg.kind = Geometry::CollisionPieceKind::Capsule;
    leg.position = glm::vec3(0.5f, 0.25f, 0.5f);
    leg.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.f, 0.f, 1.f));
    leg.radius = 0.1f;
    leg.halfHeight = 0.4f;
    collision.pieces.push_back(leg);
    return collision;
}

} // namespace

TEST_CASE("ColliderBlueprintParts: one child per piece, where the model put it")
{
    const std::vector<Editor::ColliderPart> parts = Editor::ColliderBlueprintParts(ModelId(), Chair());
    REQUIRE(parts.size() == 2);

    // The hull stays the model's, shared, as its own piece of it.
    const Editor::ColliderPart &seat = parts[0];
    CHECK(seat.name == "UCX_Seat");
    CHECK(seat.position == glm::vec3(0.f, 1.f, 0.f));
    CHECK(seat.collider.shape == Physics::ColliderShape::Convex);
    CHECK(seat.collider.collisionAsset == ModelId());
    CHECK(seat.collider.collisionPiece == 0);

    // The capsule is a capsule, exactly, turned as the model turned it.
    const Editor::ColliderPart &leg = parts[1];
    CHECK(leg.name == "UCP_Leg");
    CHECK(leg.position == glm::vec3(0.5f, 0.25f, 0.5f));
    CHECK(leg.rotation == Chair().pieces[1].rotation);
    CHECK(leg.collider.shape == Physics::ColliderShape::Capsule);
    CHECK(leg.collider.radius == doctest::Approx(0.1f));
    CHECK(leg.collider.halfHeight == doctest::Approx(0.4f));
    CHECK(leg.collider.collisionAsset.IsNil());
}

TEST_CASE("CanMakeColliderBlueprint: only a model of several pieces is broken apart")
{
    CHECK(Editor::CanMakeColliderBlueprint(Chair()));

    Geometry::CollisionData single = Chair();
    single.pieces.pop_back();
    CHECK_FALSE(Editor::CanMakeColliderBlueprint(single));
    CHECK_FALSE(Editor::CanMakeColliderBlueprint(Geometry::CollisionData{}));
}

TEST_CASE("CollisionSummary: counts each kind of piece, or says what a model with none uses")
{
    CHECK(Editor::CollisionSummary(Chair()) == "Collision: 1 hull, 1 capsule");

    Geometry::CollisionData boxes;
    boxes.pieces.resize(2);
    boxes.pieces[0].kind = Geometry::CollisionPieceKind::Box;
    boxes.pieces[1].kind = Geometry::CollisionPieceKind::Box;
    CHECK(Editor::CollisionSummary(boxes) == "Collision: 2 boxes");

    CHECK(Editor::CollisionSummary(Geometry::CollisionData{}) ==
          "Collision: none authored. Convex uses the hull of the whole model.");
}

TEST_CASE("ColliderBlueprintPath: beside the model, named after it")
{
    CHECK(Editor::ColliderBlueprintPath("props/chair.glb") == "props/chair.abp");
    CHECK(Editor::ColliderBlueprintPath("chair.gltf") == "chair.abp");
    CHECK(Editor::ColliderBlueprintPath("props.v2/chair") == "props.v2/chair.abp");
}
