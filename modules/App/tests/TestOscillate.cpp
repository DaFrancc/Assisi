/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// Tests for App::OscillateSystem, the transform-animated movers in the perf
/// reference scenes.
///
/// What is worth testing here is not that a sine wave is a sine wave, but the
/// two properties the measurement scenes depend on: that the pose at a tick is a
/// function of that tick alone (so two runs agree, whatever the frame rate did),
/// and that it never wanders away from the authored origin no matter how long it
/// runs. An integrating implementation would pass a casual "does it move" test
/// and fail both of these.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include <Assisi/App/MotionSystems.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Window/ActionMap.hpp>

using namespace Assisi::App;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// One oscillator entity, travelling along +X about the origin given.
Assisi::ECS::Entity SpawnOscillator(World &world, const Assisi::Runtime::Oscillator &oscillator)
{
    const Assisi::ECS::Entity entity   = world.scene.Create();
    Assisi::ECS::Transform *transform = world.scene.Add<Assisi::ECS::Transform>(entity);
    transform->position                 = oscillator.origin;
    (void)world.scene.Add<Assisi::Runtime::Oscillator>(entity, oscillator);
    return entity;
}

/// Runs the system for @p ticks fixed steps and returns the final position.
glm::vec3 RunTo(World &world, Assisi::ECS::Entity entity, std::uint64_t ticks)
{
    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;
    WorldManager worlds;

    for (std::uint64_t tick = 0; tick < ticks; ++tick)
    {
        SystemContext ctx{world, kStep, tick, nullptr, &actions, events, true, &worlds};
        OscillateSystem(ctx);
    }
    const Assisi::ECS::Transform *transform = world.scene.Get<Assisi::ECS::Transform>(entity);
    REQUIRE(transform != nullptr);
    return transform->position;
}

} // namespace

TEST_CASE("OscillateSystem: the pose at a tick does not depend on how it got there")
{
    // Deliberately not a whole number of periods at the tick under test. An
    // integrator's error over an exact number of cycles sums to zero, so a tick
    // chosen on a period boundary lets a wrong implementation agree with a right
    // one by coincidence — which is exactly what an earlier version of this test
    // did.
    constexpr std::uint64_t kTick = 613;
    const Assisi::Runtime::Oscillator spec{.origin        = {5.f, 1.f, -2.f},
                                           .axis          = {1.f, 0.f, 0.f},
                                           .amplitude     = 3.f,
                                           .periodSeconds = 2.f,
                                           .phase         = 0.25f};

    constexpr float kTau        = 6.28318530717958647692f;
    const float elapsed       = static_cast<float>(kTick) * kStep;
    const float expectedOffset = spec.amplitude * std::sin(kTau * (elapsed / spec.periodSeconds + spec.phase));
    // Guards the choice of tick: on a stationary point of the wave both a right
    // and a wrong implementation would sit at the origin and agree.
    REQUIRE(std::abs(expectedOffset) > 0.5f);

    // Every tick from 0 up to the one under test, versus a single evaluation at
    // it. Both are checked against the closed form rather than only against each
    // other, so neither an integrator nor a pair of matching wrong answers pass.
    WorldManager steppedWorlds;
    World &stepped                   = steppedWorlds.Create("Stepped");
    const Assisi::ECS::Entity a = SpawnOscillator(stepped, spec);
    const glm::vec3 walked           = RunTo(stepped, a, kTick + 1);

    WorldManager jumpedWorlds;
    World &jumped                    = jumpedWorlds.Create("Jumped");
    const Assisi::ECS::Entity b = SpawnOscillator(jumped, spec);

    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;
    WorldManager worlds;
    SystemContext ctx{jumped, kStep, kTick, nullptr, &actions, events, true, &worlds};
    OscillateSystem(ctx);
    const Assisi::ECS::Transform *transform = jumped.scene.Get<Assisi::ECS::Transform>(b);
    REQUIRE(transform != nullptr);

    CHECK(walked.x == doctest::Approx(spec.origin.x + expectedOffset));
    CHECK(transform->position.x == doctest::Approx(spec.origin.x + expectedOffset));
    CHECK(walked.y == doctest::Approx(transform->position.y));
    CHECK(walked.z == doctest::Approx(transform->position.z));
}

TEST_CASE("OscillateSystem: travel stays bounded around the authored origin")
{
    const glm::vec3 origin{5.f, 1.f, -2.f};
    constexpr float kAmplitude = 3.f;

    WorldManager worlds;
    World &world                = worlds.Create("Bounded");
    const Assisi::ECS::Entity entity = SpawnOscillator(world, {.origin        = origin,
                                                               .axis          = {1.f, 0.f, 0.f},
                                                               .amplitude     = kAmplitude,
                                                               .periodSeconds = 2.f,
                                                               .phase         = 0.f});

    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;
    WorldManager contextWorlds;

    float lowest  = origin.x;
    float highest = origin.x;
    // Ten minutes of simulated travel — long enough that a per-step integrator's
    // drift would be plainly visible against a 3 m amplitude.
    for (std::uint64_t tick = 0; tick < 36000; ++tick)
    {
        SystemContext ctx{world, kStep, tick, nullptr, &actions, events, true, &contextWorlds};
        OscillateSystem(ctx);

        const Assisi::ECS::Transform *transform = world.scene.Get<Assisi::ECS::Transform>(entity);
        REQUIRE(transform != nullptr);
        lowest  = std::min(lowest, transform->position.x);
        highest = std::max(highest, transform->position.x);

        // The off-axis components never move at all.
        CHECK(transform->position.y == doctest::Approx(origin.y));
        CHECK(transform->position.z == doctest::Approx(origin.z));
    }

    CHECK(lowest == doctest::Approx(origin.x - kAmplitude).epsilon(0.01));
    CHECK(highest == doctest::Approx(origin.x + kAmplitude).epsilon(0.01));
}

TEST_CASE("OscillateSystem: a parked mover is left where it was authored")
{
    const glm::vec3 origin{1.f, 2.f, 3.f};

    WorldManager worlds;
    World &world = worlds.Create("Parked");

    // Two ways to park one: no period, and no axis.
    const Assisi::ECS::Entity noPeriod = SpawnOscillator(
        world, {.origin = origin, .axis = {1.f, 0.f, 0.f}, .amplitude = 5.f, .periodSeconds = 0.f, .phase = 0.f});
    const Assisi::ECS::Entity noAxis = SpawnOscillator(
        world, {.origin = origin, .axis = {0.f, 0.f, 0.f}, .amplitude = 5.f, .periodSeconds = 2.f, .phase = 0.f});

    const glm::vec3 restedPeriod = RunTo(world, noPeriod, 120);
    const glm::vec3 restedAxis   = world.scene.Get<Assisi::ECS::Transform>(noAxis)->position;

    CHECK(restedPeriod.x == doctest::Approx(origin.x));
    CHECK(restedAxis.x == doctest::Approx(origin.x));
}

TEST_CASE("OscillateSystem: phase separates movers that share a period")
{
    const Assisi::Runtime::Oscillator base{
        .origin = {}, .axis = {1.f, 0.f, 0.f}, .amplitude = 2.f, .periodSeconds = 4.f, .phase = 0.f};

    WorldManager worlds;
    World &world = worlds.Create("Phased");

    Assisi::Runtime::Oscillator offset = base;
    offset.phase                        = 0.5f; // half a cycle out

    const Assisi::ECS::Entity first  = SpawnOscillator(world, base);
    const Assisi::ECS::Entity second = SpawnOscillator(world, offset);

    Assisi::Core::EventQueue events;
    Assisi::Window::ActionMap actions;
    WorldManager contextWorlds;
    SystemContext ctx{world, kStep, /*simTick=*/ 30, nullptr, &actions, events, true, &contextWorlds};
    OscillateSystem(ctx);

    const float a = world.scene.Get<Assisi::ECS::Transform>(first)->position.x;
    const float b = world.scene.Get<Assisi::ECS::Transform>(second)->position.x;

    // Half a cycle apart is exact opposition, which is also the strongest
    // evidence the phase term reaches the evaluation at all.
    CHECK(a == doctest::Approx(-b));
    CHECK(std::abs(a) > 0.1f); // not both parked at zero, which would satisfy the above vacuously
}
