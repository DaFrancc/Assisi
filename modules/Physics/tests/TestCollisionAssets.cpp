/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCollisionAssets.cpp
/// @brief Colliders built from a model's collision: a hull, a triangle mesh,
/// a set of pieces or one of them, shared between every collider naming the
/// same model, and refused where a body cannot take them.

#include <doctest/doctest.h>

#include "LogCapture.hpp"
#include "PhysicsTestScene.hpp"

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/CollisionData.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <vector>

using namespace Assisi;
using namespace Assisi::PhysicsTests;

namespace
{

/// The tolerance a resting height or a hit is compared at (m): a body at rest
/// sits within the solver's allowed overlap of where it would touch exactly.
constexpr double kRestTolerance = 0.03;

/// Long enough for a body dropped a metre to land and come to rest.
constexpr int32_t kSettleSteps = 240;

/// How fast a body may still drift and count as at rest (m/s).
constexpr float kRestSpeed = 0.05f;

Core::AssetId IdOf(std::uint8_t n)
{
    Core::AssetId id;
    id.bytes[0] = 0xC0;
    id.bytes[15] = n;
    return id;
}

const Core::AssetId kCube = IdOf(1);
const Core::AssetId kFloor = IdOf(2);
const Core::AssetId kTwoParts = IdOf(3);
const Core::AssetId kRoundSet = IdOf(4);
const Core::AssetId kMissing = IdOf(5);

/// A cube of half extent @p half around the origin, as a model with no pieces
/// of its own: its whole-model hull is the cube.
Geometry::CollisionModel CubeModel(float half)
{
    Geometry::CollisionModel model;
    for (std::int32_t corner = 0; corner < 8; ++corner)
    {
        model.positions.emplace_back((corner & 1) != 0 ? half : -half, (corner & 2) != 0 ? half : -half,
                                     (corner & 4) != 0 ? half : -half);
    }
    // Two triangles per face; the hull ignores winding.
    model.indices = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                     2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    return model;
}

/// A flat square of half width @p half at height 0, facing up.
Geometry::CollisionModel FloorModel(float half)
{
    Geometry::CollisionModel model;
    model.positions = {{-half, 0.f, -half}, {-half, 0.f, half}, {half, 0.f, half}, {half, 0.f, -half}};
    model.indices = {0, 1, 2, 0, 2, 3};
    return model;
}

/// A hull cube at x = -1 and a box at x = +1, each a metre across, with a gap
/// between them.
Geometry::CollisionModel TwoPartModel()
{
    Geometry::CollisionModel model = CubeModel(0.5f);
    Geometry::CollisionPiece hull;
    hull.name = "UCX_Left";
    hull.kind = Geometry::CollisionPieceKind::Hull;
    hull.position = glm::vec3(-1.f, 0.f, 0.f);
    hull.firstPoint = 0;
    hull.pointCount = 8;
    model.collision.points = model.positions;
    model.collision.pieces.push_back(hull);

    Geometry::CollisionPiece box;
    box.name = "UBX_Right";
    box.kind = Geometry::CollisionPieceKind::Box;
    box.position = glm::vec3(1.f, 0.f, 0.f);
    box.halfExtents = glm::vec3(0.5f);
    model.collision.pieces.push_back(box);
    return model;
}

/// A sphere and a box: a set with a round piece in it.
Geometry::CollisionModel RoundSetModel()
{
    Geometry::CollisionModel model = CubeModel(0.5f);
    Geometry::CollisionPiece ball;
    ball.name = "USP_Ball";
    ball.kind = Geometry::CollisionPieceKind::Sphere;
    ball.radius = 0.5f;
    model.collision.pieces.push_back(ball);

    Geometry::CollisionPiece box;
    box.name = "UBX_Block";
    box.kind = Geometry::CollisionPieceKind::Box;
    box.position = glm::vec3(2.f, 0.f, 0.f);
    box.halfExtents = glm::vec3(0.5f);
    model.collision.pieces.push_back(box);
    return model;
}

void AddModels(TestCollisionSource &collision)
{
    collision.Add(kCube, CubeModel(0.5f));
    collision.Add(kFloor, FloorModel(20.f));
    collision.Add(kTwoParts, TwoPartModel());
    collision.Add(kRoundSet, RoundSetModel());
}

Physics::Collider ModelCollider(Core::AssetId asset, Physics::ColliderShape shape)
{
    Physics::Collider collider;
    collider.shape = shape;
    collider.collisionAsset = asset;
    return collider;
}

/// A body of @p collider at @p position, moving as @p motion unless static.
ECS::Entity AddModelBody(ECS::Scene &scene, glm::vec3 position, const Physics::Collider &collider,
                         std::optional<Physics::MotionType> motion)
{
    BodySpec spec;
    spec.collider = collider;
    if (motion.has_value())
    {
        spec.rigidBody = Physics::RigidBody{};
        spec.rigidBody->motion = *motion;
    }
    return AddBody(scene, position, spec);
}

/// Where a ray straight down from high over (@p x, @p z) first meets anything.
std::optional<Physics::QueryHit> DropRay(const Physics::PhysicsWorld &world, float x, float z)
{
    constexpr float kHeight = 10.f;
    return world.CastRay(glm::vec3(x, kHeight, z), glm::vec3(0.f, -2.f * kHeight, 0.f), Physics::CollisionFilter{},
                         ECS::NullEntity);
}

void CheckAtRest(const ECS::Scene &scene, ECS::Entity entity, float height)
{
    CHECK(scene.Get<ECS::Transform>(entity)->position.y == doctest::Approx(height).epsilon(kRestTolerance));
    CHECK(glm::length(scene.Get<Physics::BodyState>(entity)->linearVelocity) < kRestSpeed);
}

} // namespace

TEST_CASE("Collision models: a dynamic convex body settles on a static triangle-mesh floor")
{
    TestScene test;
    AddModels(test.collision);
    (void)AddModelBody(test.scene, glm::vec3(0.f), ModelCollider(kFloor, Physics::ColliderShape::Mesh), std::nullopt);
    const ECS::Entity cube = AddModelBody(test.scene, glm::vec3(0.f, 1.5f, 0.f),
                                          ModelCollider(kCube, Physics::ColliderShape::Convex),
                                          Physics::MotionType::Dynamic);

    Step(test.world, kSettleSteps);
    CheckAtRest(test.scene, cube, 0.5f);
}

TEST_CASE("Collision models: a Mesh on a dynamic body is refused by name and built as its hull")
{
    TestScene test;
    AddModels(test.collision);
    (void)AddFloor(test.scene);

    const Tests::LogCapture log;
    const ECS::Entity cube = AddModelBody(test.scene, glm::vec3(0.f, 1.5f, 0.f),
                                          ModelCollider(kCube, Physics::ColliderShape::Mesh),
                                          Physics::MotionType::Dynamic);
    Step(test.world, kSettleSteps);

    CHECK(log.Mentions(std::format("entity {} (gen {}) has a Mesh collider on a dynamic body", cube.index,
                                   cube.generation)));
    CHECK(test.world.Mass(cube) == doctest::Approx(Physics::kWaterDensity));
    CheckAtRest(test.scene, cube, 0.5f);
}

TEST_CASE("Collision models: a Mesh on a kinematic body holds up what lands on it")
{
    TestScene test;
    AddModels(test.collision);
    (void)AddModelBody(test.scene, glm::vec3(0.f), ModelCollider(kFloor, Physics::ColliderShape::Mesh),
                       Physics::MotionType::Kinematic);
    const ECS::Entity ball = AddBody(test.scene, glm::vec3(0.f, 1.5f, 0.f), Ball(0.5f, false));

    Step(test.world, kSettleSteps);
    CheckAtRest(test.scene, ball, 0.5f);
}

TEST_CASE("Collision models: colliders naming one model share one shape at any scale")
{
    TestScene test;
    AddModels(test.collision);
    const Physics::Collider cube = ModelCollider(kCube, Physics::ColliderShape::Convex);
    (void)AddModelBody(test.scene, glm::vec3(0.f), cube, Physics::MotionType::Dynamic);
    const ECS::Entity big = AddModelBody(test.scene, glm::vec3(5.f, 0.f, 0.f), cube, Physics::MotionType::Dynamic);
    test.scene.GetMut<ECS::Transform>(big)->scale = glm::vec3(2.f);
    test.world.Reconcile();

    CHECK(test.world.CookedShapeCount() == 1);
    CHECK(test.collision.loads == 1);

    // Density is built into the shape, so a lighter one is a shape of its own,
    // from the model already read.
    Physics::Collider light = cube;
    light.density = Physics::kWaterDensity / 2.f;
    (void)AddModelBody(test.scene, glm::vec3(-5.f, 0.f, 0.f), light, Physics::MotionType::Dynamic);
    test.world.Reconcile();

    CHECK(test.world.CookedShapeCount() == 2);
    CHECK(test.collision.loads == 1);
}

TEST_CASE("Collision models: a hit on a model piece of a body names the piece's entity")
{
    TestScene test;
    AddModels(test.collision);
    BodySpec owner = Box(glm::vec3(0.5f), false);
    owner.rigidBody->gravityScale = 0.f;
    const ECS::Entity body = AddBody(test.scene, glm::vec3(0.f), owner);
    const ECS::Entity piece = AddChildCollider(test.scene, body, glm::vec3(0.f, 0.f, 3.f),
                                               ModelCollider(kCube, Physics::ColliderShape::Convex));
    test.world.Reconcile();

    const std::optional<Physics::QueryHit> hit = DropRay(test.world, 0.f, 3.f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == body);
    CHECK(hit->piece == piece);

    const std::optional<Physics::QueryHit> own = DropRay(test.world, 0.f, 0.f);
    REQUIRE(own.has_value());
    CHECK(own->piece == body);
}

TEST_CASE("Collision models: a set is its pieces where the model put them, weighed by volume")
{
    TestScene test;
    AddModels(test.collision);
    Physics::RigidBody floating;
    floating.gravityScale = 0.f;
    BodySpec spec;
    spec.collider = ModelCollider(kTwoParts, Physics::ColliderShape::Convex);
    spec.rigidBody = floating;
    const ECS::Entity set = AddBody(test.scene, glm::vec3(0.f), spec);
    test.world.Reconcile();

    const std::optional<Physics::QueryHit> left = DropRay(test.world, -1.f, 0.f);
    const std::optional<Physics::QueryHit> right = DropRay(test.world, 1.f, 0.f);
    REQUIRE(left.has_value());
    REQUIRE(right.has_value());
    CHECK(left->position.y == doctest::Approx(0.5f).epsilon(kRestTolerance));
    CHECK(right->position.y == doctest::Approx(0.5f).epsilon(kRestTolerance));
    CHECK_FALSE(DropRay(test.world, 0.f, 0.f).has_value());

    // Two cubic metres of water.
    CHECK(test.world.Mass(set) == doctest::Approx(2.f * Physics::kWaterDensity).epsilon(0.01));
}

TEST_CASE("Collision models: collisionPiece builds that one piece alone")
{
    TestScene test;
    AddModels(test.collision);
    Physics::Collider collider = ModelCollider(kTwoParts, Physics::ColliderShape::Convex);
    collider.collisionPiece = 1;
    (void)AddModelBody(test.scene, glm::vec3(0.f), collider, std::nullopt);
    test.world.Reconcile();

    CHECK_FALSE(DropRay(test.world, -1.f, 0.f).has_value());
    CHECK(DropRay(test.world, 1.f, 0.f).has_value());
}

TEST_CASE("Collision models: a model that cannot be read leaves its collider out, and is asked for once")
{
    TestScene test;
    AddModels(test.collision);
    const ECS::Entity lost = AddModelBody(test.scene, glm::vec3(0.f), ModelCollider(kMissing, Physics::ColliderShape::Convex),
                                          Physics::MotionType::Dynamic);
    Step(test.world, 2);

    CHECK_FALSE(test.world.HasBody(lost));
    CHECK(test.collision.loads == 1);
}

TEST_CASE("Collision models: a convex model takes the entity's scale")
{
    TestScene test;
    AddModels(test.collision);
    const ECS::Entity cube =
        AddModelBody(test.scene, glm::vec3(0.f), ModelCollider(kCube, Physics::ColliderShape::Convex), std::nullopt);
    test.scene.GetMut<ECS::Transform>(cube)->scale = glm::vec3(2.f, 1.f, 1.f);
    test.world.Reconcile();

    CHECK(DropRay(test.world, 0.9f, 0.f).has_value());
    CHECK_FALSE(DropRay(test.world, 1.1f, 0.f).has_value());
}

TEST_CASE("Collision models: a set with a round piece is built at a scale it can take")
{
    TestScene test;
    AddModels(test.collision);
    Physics::RigidBody floating;
    floating.gravityScale = 0.f;
    BodySpec spec;
    spec.collider = ModelCollider(kRoundSet, Physics::ColliderShape::Convex);
    spec.rigidBody = floating;
    const ECS::Entity set = AddBody(test.scene, glm::vec3(0.f), spec);
    test.scene.GetMut<ECS::Transform>(set)->scale = glm::vec3(2.f, 1.f, 1.f);
    Step(test.world, 2);

    REQUIRE(test.world.HasBody(set));
    const glm::vec3 built = test.world.GetColliderScale(set);
    CHECK(built.x == doctest::Approx(built.y));
    CHECK(built.y == doctest::Approx(built.z));
}

TEST_CASE("Collision models: a shape cast with a convex model collider finds what it would hit")
{
    TestScene test;
    AddModels(test.collision);
    (void)AddFloor(test.scene);
    test.world.Reconcile();

    const Physics::Collider cube = ModelCollider(kCube, Physics::ColliderShape::Convex);
    const std::optional<Physics::QueryHit> hit = test.world.CastShape(
        cube, Physics::Pose{glm::quat(1.f, 0.f, 0.f, 0.f), glm::vec3(0.f, 5.f, 0.f)}, glm::vec3(0.f, -10.f, 0.f),
        ECS::NullEntity);
    REQUIRE(hit.has_value());
    // The cube's bottom meets the floor's top after 4.5 m of the 10 m sweep.
    CHECK(hit->distance == doctest::Approx(4.5f).epsilon(kRestTolerance));
}

TEST_CASE("Collision models: a hull's outline is its edges, without the diagonals of its faces")
{
    TestScene test;
    AddModels(test.collision);
    std::vector<glm::vec3> edges;
    test.world.CollisionAssetEdges(ModelCollider(kCube, Physics::ColliderShape::Convex), edges);

    // Twelve edges, two points each.
    CHECK(edges.size() == 24);
}
