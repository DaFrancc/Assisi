/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacter.cpp
/// @brief The character controller's mechanics: what stops it, what it climbs,
///        what it slides off, and what the rest of the world can see of it.
///
/// These run a real simulation rather than faking a sweep, because the things
/// most likely to be wrong are exactly the things a fake would paper over: which
/// end of the capsule the entity's Transform is at, whether the sweep's filter is
/// the one the descriptor asked for, and whether a character exists at all to a
/// ray or a trigger volume.
///
/// The filter and visibility cases matter more than the geometry ones. A
/// character that stops a centimetre short of a wall is nobody's bug; one that
/// walks through the one wall it was told to respect, or that a bullet passes
/// straight through, is the whole feature not working.

#include <doctest/doctest.h>

#include <cstdint>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// Long enough for a fall of a couple of metres to land and settle.
constexpr int32_t kSettleSteps = 120;

/// MoveCharacter takes a velocity, not a direction — scaling a direction by a
/// speed is the App system's job. This is the descriptor's default walk speed,
/// spelled as a velocity along +x.
constexpr glm::vec3 kWalkForward{5.f, 0.f, 0.f};

/// A static box with an entity, so a hit or a contact has something to name.
ECS::Entity SpawnBox(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at,
                     glm::vec3 halfExtents, bool isStatic = true,
                     Physics::CollisionChannel channel = Physics::CollisionChannel::World)
{
    const ECS::Entity entity  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;

    Physics::RigidBodyDescriptor descriptor{};
    descriptor.halfExtents = halfExtents;
    descriptor.isStatic    = isStatic;
    descriptor.channel     = channel;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(entity, descriptor) != nullptr);

    (void)world.AddBodyFromDescriptor(scene, entity, *transform, descriptor);
    return entity;
}

/// A floor spanning the origin, top surface at y = 0.
ECS::Entity SpawnFloor(ECS::Scene &scene, Physics::PhysicsWorld &world)
{
    return SpawnBox(scene, world, {0.f, -0.5f, 0.f}, {20.f, 0.5f, 20.f});
}

/// A character standing at @p at, with whatever the descriptor defaults are
/// unless the caller changed them first.
Physics::Character SpawnCharacter(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at,
                                  Physics::CharacterDescriptor descriptor = {})
{
    const ECS::Entity entity  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    transform->position = at;
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, descriptor) != nullptr);

    const auto added = world.AddCharacterFromDescriptor(scene, entity, *transform, descriptor);
    REQUIRE(added.has_value());
    return *added;
}

/// Steps the simulation, holding @p move as the character's intent throughout.
void Walk(Physics::PhysicsWorld &world, const Physics::Character &character, glm::vec3 move,
          int32_t steps)
{
    for (int32_t i = 0; i < steps; ++i)
    {
        world.MoveCharacter(character, move, /*jump=*/ false);
        world.Update(kStep);
        world.CaptureState();
    }
}

/// The one character in @p scene, as the render writeback has left it. Read
/// through the Transform rather than the simulation because that is what gameplay
/// and the editor see — a writeback that quietly skipped characters would satisfy
/// every other assertion in this file.
glm::vec3 CharacterPosition(ECS::Scene &scene, Physics::PhysicsWorld &world)
{
    world.InterpolateTransforms(scene, 1.f);

    glm::vec3 position{0.f};
    for (auto [entity, transform, character] : scene.Query<ECS::Transform, Physics::Character>())
    {
        (void)entity;
        (void)character;
        position = transform.position;
    }
    return position;
}

} // namespace

TEST_CASE("A character dropped above a floor lands on it and reports OnGround")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 2.f, 0.f});
    Walk(world, character, glm::vec3(0.f), kSettleSteps);

    const Physics::CharacterState state = world.GetCharacterState(character);
    CHECK(state.ground == Physics::GroundState::OnGround);

    // It found the floor, and can name it.
    CHECK(scene.Get<ECS::Transform>(state.groundEntity) != nullptr);

    // The Transform is at the feet, so a character standing on a floor whose top
    // is y = 0 sits at y = 0 — not at the middle of its own capsule, which is the
    // mistake a shape centred on its origin produces.
    CHECK(CharacterPosition(scene, world).y == doctest::Approx(0.f).epsilon(0.05));
}

TEST_CASE("A character walking into a wall stops at it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    // A wall whose near face is at x = 2.
    (void)SpawnBox(scene, world, {3.f, 1.f, 0.f}, {1.f, 1.f, 5.f});

    Physics::CharacterDescriptor descriptor{};
    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

    Walk(world, character, kWalkForward, kSettleSteps);

    // Stopped at the face less its own radius, give or take the padding the sweep
    // keeps off geometry.
    const glm::vec3 position = CharacterPosition(scene, world);
    CHECK(position.x < 2.f - descriptor.radius + 0.1f);
    CHECK(position.x > 1.f); // it did actually travel
}

TEST_CASE("A character whose mask excludes World walks through the wall")
{
    // The other half of the rule, and the one a filter bug hides behind: a sweep
    // that ignored the descriptor's mask would pass the case above and fail this.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);
    (void)SpawnBox(scene, world, {3.f, 1.f, 0.f}, {1.f, 1.f, 5.f});

    Physics::CharacterDescriptor descriptor{};
    descriptor.collidesWith =
        Physics::AllChannels & ~(1u << static_cast<std::uint32_t>(Physics::CollisionChannel::World));
    descriptor.gravityScale = 0.f; // nothing left to stand on once World is out

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Walk(world, character, kWalkForward, kSettleSteps);

    CHECK(CharacterPosition(scene, world).x > 4.f); // straight through where the wall is
}

TEST_CASE("A ray finds a character and names its entity, and misses once it is removed")
{
    // What the inner body is for. Without one a character is not in the broad
    // phase at all: every cast, every trigger and every other character would pass
    // through it, and CollisionChannel::Character would name nothing that exists.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f});
    Walk(world, character, glm::vec3(0.f), 10);

    const auto hit = world.CastRay({0.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(hit.has_value());

    ECS::Entity characterEntity = ECS::NullEntity;
    for (auto [entity, ch] : scene.Query<Physics::Character>())
    {
        (void)ch;
        characterEntity = entity;
    }
    REQUIRE(characterEntity != ECS::NullEntity);
    CHECK(hit->entity == characterEntity);

    world.RemoveCharacter(character);

    // Now the ray reaches past where it was, to the floor.
    const auto afterward = world.CastRay({0.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(afterward.has_value());
    CHECK(afterward->entity != characterEntity);
}

TEST_CASE("A character crosses a trigger without being stopped, and the trigger reports it")
{
    // Both halves at once, because each alone is satisfiable by the wrong build:
    // a sweep that collided with sensors would stop (and still report), and an
    // inner body missing the Trigger channel would pass through silently.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const ECS::Entity trigger = SpawnBox(scene, world, {2.f, 1.f, 0.f}, {0.5f, 1.f, 2.f},
                                         /*isStatic=*/ false, Physics::CollisionChannel::Trigger);

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f});

    bool sawCharacterEnterTrigger = false;
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        world.MoveCharacter(character, kWalkForward, /*jump=*/ false);
        world.Update(kStep);
        world.CaptureState();

        for (const Physics::ContactEvent &event : world.ContactEvents())
        {
            if (event.other == trigger && event.sensor)
            {
                sawCharacterEnterTrigger = true;
            }
        }
    }

    CHECK(sawCharacterEnterTrigger);

    // It went past rather than stopping at the volume.
    CHECK(CharacterPosition(scene, world).x > 3.f);
}

TEST_CASE("A character standing on a static floor still reports a contact with it")
{
    // The case the inner body alone cannot produce: it is kinematic, and a
    // kinematic body resting on a static one generates no Jolt contact at all.
    // The character's own sweep is what sees this, so a build that reported
    // contacts only through the inner body fails here and nowhere else.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    const ECS::Entity floor = SpawnFloor(scene, world);

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.5f, 0.f});

    bool touchedFloor = false;
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        world.MoveCharacter(character, glm::vec3(0.f), /*jump=*/ false);
        world.Update(kStep);
        world.CaptureState();

        for (const Physics::ContactEvent &event : world.ContactEvents())
        {
            if (event.other == floor)
            {
                touchedFloor = true;
            }
        }
    }

    CHECK(touchedFloor);
}

TEST_CASE("A step within maxStepHeight is climbed, and a taller one is not")
{
    Physics::CharacterDescriptor descriptor{};
    descriptor.maxStepHeight = 0.4f;

    const auto climbTo = [&descriptor](float stepHeight)
    {
        ECS::Scene scene;
        Physics::PhysicsWorld world;
        (void)SpawnFloor(scene, world);

        // A block whose top is at stepHeight, starting at x = 1 and running well
        // past where the character can walk in the time below — it has to still
        // be standing on the step at the end, not off the far side of it.
        SpawnBox(scene, world, {31.f, stepHeight * 0.5f, 0.f}, {30.f, stepHeight * 0.5f, 5.f});

        const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Walk(world, character, kWalkForward, kSettleSteps);

        return CharacterPosition(scene, world).y;
    };

    // Comfortably under the limit: it steps up and is standing on top.
    CHECK(climbTo(0.3f) == doctest::Approx(0.3f).epsilon(0.15));

    // Well over it: the block is a wall, and the character stays on the floor.
    CHECK(climbTo(0.9f) == doctest::Approx(0.f).epsilon(0.1));
}

TEST_CASE("A slope steeper than maxSlopeDegrees is not walkable")
{
    // Jolt reports a too-steep surface as OnSteepGround rather than OnGround,
    // which is what the movement code branches on: a character there slides
    // instead of walking, and must not be allowed to jump off it.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.maxSlopeDegrees = 30.f;

    // A box rolled well past 30 degrees about Z, so its upper face is too steep.
    const ECS::Entity ramp       = scene.Create();
    ECS::Transform   *rampTransform = scene.Add<ECS::Transform>(ramp);
    REQUIRE(rampTransform != nullptr);
    rampTransform->position = {2.f, 0.f, 0.f};
    rampTransform->rotation = glm::angleAxis(glm::radians(60.f), glm::vec3(0.f, 0.f, 1.f));

    Physics::RigidBodyDescriptor rampDescriptor{};
    rampDescriptor.halfExtents = {2.f, 0.5f, 5.f};
    rampDescriptor.isStatic    = true;
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(ramp, rampDescriptor) != nullptr);
    (void)world.AddBodyFromDescriptor(scene, ramp, *rampTransform, rampDescriptor);

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Walk(world, character, kWalkForward, kSettleSteps);

    // It never gets up the face: without the slope limit it would walk up a
    // 60-degree surface as though it were a ramp.
    CHECK(CharacterPosition(scene, world).y < 1.f);
}

TEST_CASE("A character crouches under a low ceiling and cannot stand until it is clear")
{
    // The whole reason SetCharacterStance answers rather than commands.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.radius           = 0.3f;
    descriptor.halfHeight       = 0.6f;
    descriptor.crouchHalfHeight = 0.1f;

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

    // Standing is 2*(0.6+0.3) = 1.8 tall; crouching is 2*(0.1+0.3) = 0.8. A
    // ceiling whose underside is at 1.0 admits only the crouch.
    (void)SpawnBox(scene, world, {0.f, 1.5f, 0.f}, {2.f, 0.5f, 2.f});

    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Crouching));
    Walk(world, character, glm::vec3(0.f), 10);

    // Asked to stand where it cannot: refused, and the stance is unchanged.
    CHECK_FALSE(world.SetCharacterStance(character, Physics::Stance::Standing));
    CHECK(world.GetCharacterState(character).stance == Physics::Stance::Crouching);

    // Walk out from under it, then the same request succeeds.
    Walk(world, character, kWalkForward, kSettleSteps);
    CHECK(world.SetCharacterStance(character, Physics::Stance::Standing));
    CHECK(world.GetCharacterState(character).stance == Physics::Stance::Standing);
}

TEST_CASE("A crouched character is not hit at standing head height")
{
    // The inner body has to be reshaped alongside the sweep shape, or a crouched
    // character is still shot in the head.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.radius           = 0.3f;
    descriptor.halfHeight       = 0.6f;
    descriptor.crouchHalfHeight = 0.1f;

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Walk(world, character, glm::vec3(0.f), 10);

    // Across the standing capsule's upper half, well above the crouched one.
    const glm::vec3 origin{-3.f, 1.5f, 0.f};
    const glm::vec3 sweep{6.f, 0.f, 0.f};

    REQUIRE(world.CastRay(origin, sweep, {}, ECS::NullEntity).has_value());

    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Crouching));
    Walk(world, character, glm::vec3(0.f), 10);

    CHECK_FALSE(world.CastRay(origin, sweep, {}, ECS::NullEntity).has_value());
}

TEST_CASE("A teleported character is somewhere else immediately, not next step")
{
    // A cast made between the teleport and the next Update has to find it at the
    // destination: the inner body is a separate object and does not follow on its
    // own.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f});
    Walk(world, character, glm::vec3(0.f), 10);

    world.SetCharacterTransform(character, {10.f, 0.f, 0.f}, glm::quat{1.f, 0.f, 0.f, 0.f});

    // Nothing is left standing where it was: the ray reaches the floor instead.
    const auto atOldPlace = world.CastRay({0.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(atOldPlace.has_value());
    CHECK(atOldPlace->position.y == doctest::Approx(0.f).epsilon(0.05));

    const auto atNewPlace = world.CastRay({10.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(atNewPlace.has_value());
    CHECK(atNewPlace->position.y > 0.5f); // stopped on the character, above the floor

    // And the render pose arrives rather than sliding: both snapshot halves were
    // collapsed onto the target, so even a half-blend reads the destination.
    world.InterpolateTransforms(scene, 0.5f);
    for (auto [entity, transform, ch] : scene.Query<ECS::Transform, Physics::Character>())
    {
        (void)entity;
        (void)ch;
        CHECK(transform.position.x == doctest::Approx(10.f).epsilon(0.01));
    }
}

TEST_CASE("A frozen character does not fall, and falls again when released")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const Physics::Character character = SpawnCharacter(scene, world, {0.f, 5.f, 0.f});

    world.SetCharacterFrozen(character, true);
    Walk(world, character, glm::vec3(0.f), kSettleSteps);
    CHECK(CharacterPosition(scene, world).y == doctest::Approx(5.f).epsilon(0.01));

    world.SetCharacterFrozen(character, false);
    Walk(world, character, glm::vec3(0.f), kSettleSteps);
    CHECK(CharacterPosition(scene, world).y == doctest::Approx(0.f).epsilon(0.05));
}

TEST_CASE("Two characters walking at each other stop rather than overlapping")
{
    // A character is not in the broad phase, so this only works because each one's
    // sweep meets the other's inner body.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.radius = 0.3f;

    const Physics::Character left  = SpawnCharacter(scene, world, {-2.f, 0.f, 0.f}, descriptor);
    const Physics::Character right = SpawnCharacter(scene, world, {2.f, 0.f, 0.f}, descriptor);

    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        world.MoveCharacter(left, {1.f, 0.f, 0.f}, /*jump=*/ false);
        world.MoveCharacter(right, {-1.f, 0.f, 0.f}, /*jump=*/ false);
        world.Update(kStep);
        world.CaptureState();
    }

    world.InterpolateTransforms(scene, 1.f);

    std::vector<float> xs;
    for (auto [entity, tc, ch] : scene.Query<ECS::Transform, Physics::Character>())
    {
        (void)entity;
        (void)ch;
        xs.push_back(tc.position.x);
    }
    REQUIRE(xs.size() == 2u);

    // Still apart by about their two radii, rather than having walked through one
    // another and swapped sides.
    CHECK(std::abs(xs[0] - xs[1]) > 2.f * descriptor.radius * 0.8f);
}

TEST_CASE("RebuildSceneBodies builds one character per descriptor, and twice leaves one")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity entity  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, Physics::CharacterDescriptor{}) != nullptr);

    world.RebuildSceneBodies(scene);
    REQUIRE(scene.Get<Physics::Character>(entity) != nullptr);

    // Again, as a level reload would: the first set has to be torn down rather
    // than leaked, and Jolt asserts at teardown if a character outlives its world.
    world.RebuildSceneBodies(scene);
    CHECK(scene.Get<Physics::Character>(entity) != nullptr);
}

TEST_CASE("An entity carrying both descriptors gets neither")
{
    // A character already owns a rigid body. Building both would have it collide
    // with its own, so the pair is refused rather than resolved by whichever
    // branch happened to run last.
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity entity  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, Physics::CharacterDescriptor{}) != nullptr);
    REQUIRE(scene.Add<Physics::RigidBodyDescriptor>(entity, Physics::RigidBodyDescriptor{}) != nullptr);

    const auto built = world.RebuildEntityPhysics(scene, entity);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == Physics::PhysicsWorld::PhysicsError::ConflictingDescriptors);
    CHECK(scene.Get<Physics::Character>(entity) == nullptr);
    CHECK(scene.Get<Physics::RigidBody>(entity) == nullptr);
}

TEST_CASE("RebuildEntityPhysics needs a Transform to place anything at")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world;

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, Physics::CharacterDescriptor{}) != nullptr);

    const auto built = world.RebuildEntityPhysics(scene, entity);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == Physics::PhysicsWorld::PhysicsError::NoTransform);
}

TEST_CASE("RemoveEntityPhysics works after the handle component is already gone")
{
    // The play/stop teardown path: component destruction is deferred, so the
    // handle can be missing while the simulated object is still there. A lookup
    // that went through the scene would leak it.
    ECS::Scene scene;
    Physics::PhysicsWorld world;
    (void)SpawnFloor(scene, world);

    const ECS::Entity entity  = scene.Create();
    ECS::Transform   *transform = scene.Add<ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, Physics::CharacterDescriptor{}) != nullptr);
    REQUIRE(world.RebuildEntityPhysics(scene, entity).has_value());

    world.Update(kStep);
    world.CaptureState();
    REQUIRE(world.CastRay({0.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity)->entity == entity);

    // Drop the handle behind the world's back, then ask it to clean up.
    scene.Remove<Physics::Character>(entity);
    world.RemoveEntityPhysics(scene, entity);

    const auto afterward = world.CastRay({0.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(afterward.has_value());
    CHECK(afterward->entity != entity);
}
