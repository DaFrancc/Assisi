/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCharacterOrder.cpp
/// @brief Characters step in an order two machines agree on, and a subset of
///        them can be stepped exactly as a whole step steps them.
///
/// Two characters walking into each other end where they do partly because of
/// which one swept first. A host and a client hold the same two under different
/// entity indices, so an order by index has them disagree for as long as the
/// two touch.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>

#include "PhysicsTestScene.hpp"

using namespace Assisi;
using Assisi::PhysicsTests::kStep;

namespace
{

/// Steps two characters walk into each other for: long enough to meet and
/// shove for most of a second.
constexpr int32_t kShoveSteps = 90;

/// Steps a character is given to come to rest on the floor.
constexpr int32_t kSettleSteps = 30;

/// How far apart the two start (m), head on.
constexpr float kStartApart = 2.f;

/// How far two runs of the same steps may land apart (m). Both run the same
/// arithmetic in the same order, so only rounding.
constexpr float kSameRunTolerance = 1e-5f;

/// Walking speeds asked of each (m/s): unequal, so one shoves the other back.
constexpr float kLeftSpeed = 3.f;
constexpr float kRightSpeed = 1.f;

/// The order keys the two are given, as a session would by network id.
constexpr std::uint64_t kLeftKey = 1;
constexpr std::uint64_t kRightKey = 2;

struct Pair
{
    ECS::Entity left{ECS::NullEntity};
    ECS::Entity right{ECS::NullEntity};
};

/// Two characters on a floor, made right first when @p rightFirst, so their
/// entity indices are the other way round; each keyed as a session keys them.
Pair MakePair(PhysicsTests::TestScene &test, bool rightFirst)
{
    (void)PhysicsTests::AddFloor(test.scene);
    const glm::vec3 leftFeet{-kStartApart / 2.f, 0.f, 0.f};
    const glm::vec3 rightFeet{kStartApart / 2.f, 0.f, 0.f};
    Pair pair;
    if (rightFirst)
    {
        pair.right = PhysicsTests::AddCharacter(test.scene, rightFeet);
        pair.left = PhysicsTests::AddCharacter(test.scene, leftFeet);
    }
    else
    {
        pair.left = PhysicsTests::AddCharacter(test.scene, leftFeet);
        pair.right = PhysicsTests::AddCharacter(test.scene, rightFeet);
    }
    test.world.SetCharacterOrderKey(pair.left, kLeftKey);
    test.world.SetCharacterOrderKey(pair.right, kRightKey);
    test.world.Reconcile();
    PhysicsTests::Step(test.world, kSettleSteps);
    return pair;
}

/// Both walk at each other for one step's intent.
void WalkAtEachOther(PhysicsTests::TestScene &test, const Pair &pair)
{
    PhysicsTests::Drive(test.scene, pair.left, {kLeftSpeed, 0.f, 0.f}, /*jump=*/ false);
    PhysicsTests::Drive(test.scene, pair.right, {-kRightSpeed, 0.f, 0.f}, /*jump=*/ false);
}

glm::vec3 FeetOf(const PhysicsTests::TestScene &test, ECS::Entity character)
{
    return test.scene.Get<ECS::Transform>(character)->position;
}

} // namespace

TEST_CASE("Two characters shoving each other end alike on machines whose entity indices differ")
{
    PhysicsTests::TestScene host;
    PhysicsTests::TestScene client;
    const Pair onHost = MakePair(host, /*rightFirst=*/ false);
    const Pair onClient = MakePair(client, /*rightFirst=*/ true);
    REQUIRE(onHost.left.index != onClient.left.index);

    for (int32_t i = 0; i < kShoveSteps; ++i)
    {
        WalkAtEachOther(host, onHost);
        WalkAtEachOther(client, onClient);
        host.world.Update(kStep);
        client.world.Update(kStep);
    }

    // They met: neither got where walking alone would have put it.
    CHECK(FeetOf(host, onHost.left).x < FeetOf(host, onHost.right).x);

    CHECK(glm::length(FeetOf(host, onHost.left) - FeetOf(client, onClient.left)) < kSameRunTolerance);
    CHECK(glm::length(FeetOf(host, onHost.right) - FeetOf(client, onClient.right)) < kSameRunTolerance);
}

TEST_CASE("Stepping only the characters steps them as a whole step does")
{
    // Nothing but the characters moves here, so the rest of a step has nothing
    // to do, and the two must agree exactly; they push each other on the way.
    PhysicsTests::TestScene whole;
    PhysicsTests::TestScene subset;
    const Pair inWhole = MakePair(whole, /*rightFirst=*/ false);
    const Pair inSubset = MakePair(subset, /*rightFirst=*/ false);
    const std::array<ECS::Entity, 2> both{inSubset.right, inSubset.left};

    for (int32_t i = 0; i < kShoveSteps; ++i)
    {
        WalkAtEachOther(whole, inWhole);
        WalkAtEachOther(subset, inSubset);
        whole.world.Update(kStep);
        subset.world.StepCharacters(both, kStep);
    }

    CHECK(glm::length(FeetOf(whole, inWhole.left) - FeetOf(subset, inSubset.left)) < kSameRunTolerance);
    CHECK(glm::length(FeetOf(whole, inWhole.right) - FeetOf(subset, inSubset.right)) < kSameRunTolerance);
}
