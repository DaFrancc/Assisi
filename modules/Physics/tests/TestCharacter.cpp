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

#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Math/Matrix.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::kStep;

namespace
{

/// Long enough for a fall of a couple of metres to land and settle.
constexpr int32_t kSettleSteps = 120;

/// MoveCharacter takes a direction with a speed for its length — scaling a
/// direction by the descriptor's speed is the App system's job. This asks for a
/// brisk 5 m/s along +x.
constexpr glm::vec3 kWalkForward{5.f, 0.f, 0.f};

/// A box reconciled into @p world at once, so a hit or a contact has something
/// to name before any step runs.
ECS::Entity SpawnBox(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at, glm::vec3 halfExtents,
                     Physics::CollisionChannel channel = Physics::CollisionChannel::World)
{
    // A static box, or the always-awake kind of sensor for a trigger.
    Physics::RigidBodyDescriptor descriptor =
        PhysicsTests::Box(halfExtents, channel != Physics::CollisionChannel::Trigger);
    descriptor.channel = channel;
    const ECS::Entity entity = PhysicsTests::AddBody(scene, at, descriptor);
    world.Reconcile();
    return entity;
}

/// A floor spanning the origin, top surface at y = 0.
ECS::Entity SpawnFloor(ECS::Scene &scene, Physics::PhysicsWorld &world)
{
    return SpawnBox(scene, world, {0.f, -0.5f, 0.f}, {20.f, 0.5f, 20.f});
}

/// A character standing at @p at, with whatever the descriptor defaults are
/// unless the caller changed them first, reconciled into @p world at once.
ECS::Entity SpawnCharacter(ECS::Scene &scene, Physics::PhysicsWorld &world, glm::vec3 at,
                           Physics::CharacterDescriptor descriptor = {})
{
    const ECS::Entity entity = PhysicsTests::AddCharacter(scene, at, descriptor);
    world.Reconcile();
    REQUIRE(world.HasBody(entity));
    return entity;
}

/// Steps the simulation, holding @p move as the character's intent throughout.
void Walk(Physics::PhysicsWorld &world, ECS::Entity character, glm::vec3 move, int32_t steps)
{
    for (int32_t i = 0; i < steps; ++i)
    {
        world.MoveCharacter(character, move, /*jump=*/ false);
        world.Update(kStep);
    }
}

/// The one character in @p scene, as the writeback has left it. Read
/// through the Transform rather than the simulation because that is what gameplay
/// and the editor see — a writeback that quietly skipped characters would satisfy
/// every other assertion in this file.
glm::vec3 CharacterPosition(ECS::Scene &scene)
{
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
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 2.f, 0.f});
    Walk(world, character, glm::vec3(0.f), kSettleSteps);

    const Physics::CharacterState state = world.GetCharacterState(character);
    CHECK(state.ground == Physics::GroundState::OnGround);

    // It found the floor, and can name it.
    CHECK(scene.Get<ECS::Transform>(state.groundEntity) != nullptr);

    // The Transform is at the feet, so a character standing on a floor whose top
    // is y = 0 sits at y = 0 — not at the middle of its own capsule, which is the
    // mistake a shape centred on its origin produces.
    CHECK(CharacterPosition(scene).y == doctest::Approx(0.f).epsilon(0.05));
}

TEST_CASE("A character walking into a wall stops at it")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    // A wall whose near face is at x = 2.
    (void)SpawnBox(scene, world, {3.f, 1.f, 0.f}, {1.f, 1.f, 5.f});

    Physics::CharacterDescriptor descriptor{};
    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

    Walk(world, character, kWalkForward, kSettleSteps);

    // Stopped at the face less its own radius, give or take the padding the sweep
    // keeps off geometry.
    const glm::vec3 position = CharacterPosition(scene);
    CHECK(position.x < 2.f - descriptor.radius + 0.1f);
    CHECK(position.x > 1.f); // it did actually travel
}

TEST_CASE("A character whose mask excludes World walks through the wall")
{
    // The other half of the rule, and the one a filter bug hides behind: a sweep
    // that ignored the descriptor's mask would pass the case above and fail this.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);
    (void)SpawnBox(scene, world, {3.f, 1.f, 0.f}, {1.f, 1.f, 5.f});

    Physics::CharacterDescriptor descriptor{};
    descriptor.collidesWith = Physics::AllChannels.Without(Physics::CollisionChannel::World);
    descriptor.gravityScale = 0.f; // nothing left to stand on once World is out

    // With nothing underfoot it moves by the air rules, which hold a straight
    // request to the air cap. Lifted out of the way: this case is about the
    // mask, and needs the character to actually arrive at the wall.
    descriptor.airWishSpeedCap = 100.f;

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Walk(world, character, kWalkForward, kSettleSteps);

    CHECK(CharacterPosition(scene).x > 4.f); // straight through where the wall is
}

TEST_CASE("A ray finds a character and names its entity, and misses once it is removed")
{
    // What the inner body is for. Without one a character is not in the broad
    // phase at all: every cast, every trigger and every other character would pass
    // through it, and CollisionChannel::Character would name nothing that exists.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f});
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

    scene.Destroy(character);
    scene.FlushDestroyed();
    world.Reconcile();

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
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    const ECS::Entity trigger =
        SpawnBox(scene, world, {2.f, 1.f, 0.f}, {0.5f, 1.f, 2.f}, Physics::CollisionChannel::Trigger);

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f});

    bool sawCharacterEnterTrigger = false;
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        world.MoveCharacter(character, kWalkForward, /*jump=*/ false);
        world.Update(kStep);

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
    CHECK(CharacterPosition(scene).x > 3.f);
}

TEST_CASE("A character standing on a static floor still reports a contact with it")
{
    // The case the inner body alone cannot produce: it is kinematic, and a
    // kinematic body resting on a static one generates no Jolt contact at all.
    // The character's own sweep is what sees this, so a build that reported
    // contacts only through the inner body fails here and nowhere else.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    const ECS::Entity floor = SpawnFloor(scene, world);

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.5f, 0.f});

    bool touchedFloor = false;
    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        world.MoveCharacter(character, glm::vec3(0.f), /*jump=*/ false);
        world.Update(kStep);

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
        Physics::PhysicsWorld world{scene};
        (void)SpawnFloor(scene, world);

        // A block whose top is at stepHeight, starting at x = 1 and running well
        // past where the character can walk in the time below — it has to still
        // be standing on the step at the end, not off the far side of it.
        SpawnBox(scene, world, {31.f, stepHeight * 0.5f, 0.f}, {30.f, stepHeight * 0.5f, 5.f});

        const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
        Walk(world, character, kWalkForward, kSettleSteps);

        return CharacterPosition(scene).y;
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
    Physics::PhysicsWorld world{scene};
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
    world.Reconcile();

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Walk(world, character, kWalkForward, kSettleSteps);

    // It never gets up the face: without the slope limit it would walk up a
    // 60-degree surface as though it were a ramp.
    CHECK(CharacterPosition(scene).y < 1.f);
}

TEST_CASE("A character crouches under a low ceiling and cannot stand until it is clear")
{
    // The whole reason SetCharacterStance answers rather than commands.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.radius           = 0.3f;
    descriptor.halfHeight       = 0.6f;
    descriptor.crouchHalfHeight = 0.1f;

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);

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
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.radius           = 0.3f;
    descriptor.halfHeight       = 0.6f;
    descriptor.crouchHalfHeight = 0.1f;

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Walk(world, character, glm::vec3(0.f), 10);

    // Across the standing capsule's upper half, well above the crouched one.
    const glm::vec3 origin{-3.f, 1.5f, 0.f};
    const glm::vec3 sweep{6.f, 0.f, 0.f};

    REQUIRE(world.CastRay(origin, sweep, {}, ECS::NullEntity).has_value());

    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Crouching));
    Walk(world, character, glm::vec3(0.f), 10);

    CHECK_FALSE(world.CastRay(origin, sweep, {}, ECS::NullEntity).has_value());
}

TEST_CASE("Crouching in the air lifts the feet and leaves the eye where it was")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.radius = 0.3f;
    descriptor.halfHeight = 0.6f;
    descriptor.crouchHalfHeight = 0.3f;
    descriptor.jumpSpeed = 8.f; // high enough to stand back up before landing

    // Standing is 1.8 tall and crouching 1.2, so the feet have 0.6 to travel.
    constexpr float kLift = 0.6f;

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f}, descriptor);
    Walk(world, character, glm::vec3(0.f), 60);

    world.MoveCharacter(character, glm::vec3(0.f), /*jump=*/ true);
    world.Update(kStep);
    Walk(world, character, glm::vec3(0.f), 14);
    REQUIRE(world.GetCharacterState(character).ground == Physics::GroundState::InAir);

    const float feetBefore = CharacterPosition(scene).y;
    const float eyeBefore = feetBefore + world.GetCharacterState(character).eyeHeight;
    REQUIRE(feetBefore > kLift);

    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Crouching));

    // No step in between: the whole change is the stance call's.
    const float feetCrouched = CharacterPosition(scene).y;
    CHECK(feetCrouched == doctest::Approx(feetBefore + kLift));
    CHECK(feetCrouched + world.GetCharacterState(character).eyeHeight == doctest::Approx(eyeBefore));

    // With room below, standing puts the feet back.
    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Standing));
    CHECK(CharacterPosition(scene).y == doctest::Approx(feetBefore));
    CHECK(world.GetCharacterState(character).stance == Physics::Stance::Standing);
}

TEST_CASE("Crouching on the ground leaves the feet where they are")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f});
    Walk(world, character, glm::vec3(0.f), 60);
    const float feetBefore = CharacterPosition(scene).y;

    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Crouching));
    CHECK(CharacterPosition(scene).y == doctest::Approx(feetBefore));
}

TEST_CASE("A character crouched in the air cannot stand while the floor is too close below")
{
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.gravityScale = 0.f; // hangs where it is put

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 2.f, 0.f}, descriptor);
    Walk(world, character, glm::vec3(0.f), 2);
    REQUIRE(world.SetCharacterStance(character, Physics::Stance::Crouching));

    // Feet 0.2 above the floor: standing would put them 0.4 under it.
    constexpr float kFeetHeight = 0.2f;
    world.Teleport(character, Physics::Pose{glm::quat{1.f, 0.f, 0.f, 0.f}, {0.f, kFeetHeight, 0.f}});
    REQUIRE(world.GetCharacterState(character).ground != Physics::GroundState::OnGround);

    CHECK_FALSE(world.SetCharacterStance(character, Physics::Stance::Standing));
    CHECK(world.GetCharacterState(character).stance == Physics::Stance::Crouching);

    // Refused means untouched: left lowered, it would be a crouched capsule
    // sunk into the floor. Read after a step, so it is where the simulation
    // has the character and not where it was last drawn.
    Walk(world, character, glm::vec3(0.f), 1);
    CHECK(CharacterPosition(scene).y == doctest::Approx(kFeetHeight));
}

TEST_CASE("A teleported character is somewhere else immediately, not next step")
{
    // A cast made between the teleport and the next Update has to find it at the
    // destination: the inner body is a separate object and does not follow on its
    // own.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    const ECS::Entity character = SpawnCharacter(scene, world, {0.f, 0.f, 0.f});
    Walk(world, character, glm::vec3(0.f), 10);
    uint64_t tick = ECS::PropagateTransforms(scene, 0);

    // A step that moves it, so it is blending when the teleport lands.
    {
        const ECS::FixedStepScope step(scene);
        Walk(world, character, glm::vec3(1.f, 0.f, 0.f), 1);
    }
    REQUIRE(scene.Get<ECS::Transform>(character)->position.x > 0.f);

    world.Teleport(character, Physics::Pose{glm::quat{1.f, 0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}});

    // Nothing is left standing where it was: the ray reaches the floor instead.
    const auto atOldPlace = world.CastRay({0.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(atOldPlace.has_value());
    CHECK(atOldPlace->position.y == doctest::Approx(0.f).epsilon(0.05));

    const auto atNewPlace = world.CastRay({10.f, 5.f, 0.f}, {0.f, -10.f, 0.f}, {}, ECS::NullEntity);
    REQUIRE(atNewPlace.has_value());
    CHECK(atNewPlace->position.y > 0.5f); // stopped on the character, above the floor

    // And it is drawn there rather than slid to: even a half-blend reads the
    // destination.
    ECS::SetBlendAlpha(scene, 0.5f);
    tick = ECS::PropagateTransforms(scene, tick);
    CHECK(Math::TranslationOf(*ECS::WorldMatrix(scene, character)).x == 10.f);
}

TEST_CASE("Two characters walking at each other stop rather than overlapping")
{
    // A character is not in the broad phase, so this only works because each one's
    // sweep meets the other's inner body.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};
    (void)SpawnFloor(scene, world);

    Physics::CharacterDescriptor descriptor{};
    descriptor.radius = 0.3f;

    const ECS::Entity left  = SpawnCharacter(scene, world, {-2.f, 0.f, 0.f}, descriptor);
    const ECS::Entity right = SpawnCharacter(scene, world, {2.f, 0.f, 0.f}, descriptor);

    for (int32_t i = 0; i < kSettleSteps; ++i)
    {
        world.MoveCharacter(left, {1.f, 0.f, 0.f}, /*jump=*/ false);
        world.MoveCharacter(right, {-1.f, 0.f, 0.f}, /*jump=*/ false);
        world.Update(kStep);
    }


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

TEST_CASE("A character descriptor brings the Transform and the Character it needs")
{
    // The descriptor alone is enough to simulate: what it requires arrives with it,
    // so there is no step on which a character lacks the component its intent is
    // read from or the pose it is placed at.
    ECS::Scene scene;
    Physics::PhysicsWorld world{scene};

    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, Physics::CharacterDescriptor{}) != nullptr);
    CHECK(scene.Has<ECS::Transform>(entity));
    CHECK(scene.Has<Physics::Character>(entity));

    world.Reconcile();
    CHECK(world.HasBody(entity));
}

TEST_CASE("An entity cannot carry both descriptors")
{
    // A character already owns a rigid body. Building both would have it collide
    // with its own, so the scene refuses the second descriptor rather than leaving
    // physics to pick one.
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(scene.Add<Physics::CharacterDescriptor>(entity, Physics::CharacterDescriptor{}) != nullptr);

    CHECK(scene.ConflictOf(entity, Core::Reflect::ComponentIdOf<Physics::RigidBodyDescriptor>()).has_value());
}
