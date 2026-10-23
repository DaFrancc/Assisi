/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerSessionTesting.hpp
/// @brief A host and its clients, each a real session over loopback sockets,
///        each playing its own player, stepped frame by frame in real time.
///
/// Real sessions rather than a hand-driven server and client: what is checked is
/// that a client's controls, stamped with its own clock's tick, are the ones the
/// host's step for that tick reads, and that only the player whose controls they
/// are moves on the host.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <Assisi/App/FixedStepClock.hpp>
#include <Assisi/App/NetworkedPlay.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/NetSync/NetSession.hpp>
#include <Assisi/Physics/CollisionSource.hpp>

namespace Assisi::App::Testing
{

inline constexpr float kStep = 1.f / 60.f;

/// Steps for a client to connect, say hello and receive everything.
inline constexpr std::int32_t kJoinSteps = 150;

/// Frames a client walks for while its character is watched.
inline constexpr std::int32_t kLongWalkFrames = 120;

/// Frames that see the host step past every tick a client has stepped, and a
/// snapshot about it arrive: the client's lead and the trip back, with room.
inline constexpr std::int32_t kCatchUpFrames = 30;

/// Frames walked before a gap in commands, so the host holds a walk.
inline constexpr std::int32_t kWalkBeforeGap = 30;

/// The most fixed steps one machine runs in a frame, as the hosts cap them.
inline constexpr std::uint32_t kMaxStepsPerFrame = 4;

/// Makes the asset root @p name under the temporary directory, empty, and points
/// the asset system at it. Each file of cases names its own, since ctest runs the
/// files side by side.
std::filesystem::path FreshRoot(std::string_view name);

/// A player: a character with a camera, both sent to every client whole. The
/// client that plays it predicts both, so the host's copies of them arriving
/// late never set its own back.
void WritePlayerBlueprint(const std::filesystem::path &root);

/// One machine: its world, its session, its players, and its own fixed-step
/// clock and tick, since a client runs a little faster or slower than the host.
struct Machine
{
    /// Runs the systems the editor's play runs a player with.
    Machine();

    App::World world{Physics::NoCollisionAssets()};
    App::NetworkedPlay play{world};
    Core::EventQueue universal{Core::EventScope::Universal};
    App::FixedStepClock clock{kStep, kMaxStepsPerFrame};
    /// The tick each step was, in order: the session's, not the local count.
    std::vector<std::uint64_t> stepTicks;
    /// Called after each step with its tick, to watch what the step did.
    std::function<void(std::uint64_t)> afterStep;
    std::uint64_t tick = 0;
    /// While above zero, a step sends nothing and counts this down: the
    /// commands for those ticks never reach the host.
    std::int32_t silentSteps = 0;
};

/// One fixed step on @p machine, as every program runs one.
void Step(Machine &machine, std::uint64_t localTick);

template <typename T> std::int32_t Count(ECS::Scene &scene)
{
    std::int32_t count = 0;
    for (auto [entity, component] : scene.Query<T>())
    {
        (void)entity;
        (void)component;
        ++count;
    }
    return count;
}

/// This machine's player, or NullEntity when it has not exactly one.
ECS::Entity LocalPlayerOf(ECS::Scene &scene);

/// How far up the camera parented to @p character looks, in radians; 0 when
/// it has none.
float PitchOfEye(ECS::Scene &scene, ECS::Entity character);

/// The host's player for @p client.
ECS::Entity PlayerOf(Machine &host, NetSync::ClientId client);

/// Machines and the loop that runs them: a frame every step's worth of real
/// time, in which each machine runs as many fixed steps as its session's time
/// scale gives it, as the hosts run them.
struct Session
{
    std::vector<std::unique_ptr<Machine>> machines;

    Machine &Add();

    /// Runs @p frames frames. Real time has to match: these are real sockets,
    /// and a client's clock measures the trip in milliseconds and leads by it
    /// in ticks, so ticks run faster than real time read as commands arriving
    /// late.
    template <typename Before> void Run(std::int32_t frames, Before before)
    {
        using Clock = std::chrono::steady_clock;
        const Clock::duration frame =
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kStep));
        Clock::time_point next = Clock::now();
        for (std::int32_t i = 0; i < frames; ++i)
        {
            before();
            for (const std::unique_ptr<Machine> &machine : machines)
            {
                const std::uint32_t steps =
                    machine->clock.Advance(static_cast<double>(kStep) * machine->play.Session().TimeScale());
                for (std::uint32_t step = 0; step < steps; ++step)
                {
                    Step(*machine, ++machine->tick);
                }
                // Once a frame, after the steps, as the editor draws.
                machine->play.Session().SmoothView(kStep);
            }
            next += frame;
            std::this_thread::sleep_until(next);
        }
    }

    void Run(std::int32_t frames);
};

/// Hosts @p play on the first free port of a small range; 0 when none was.
std::uint16_t HostOnFreePort(App::NetworkedPlay &play);

/// A host and @p client on a floor, a start each for the host to make their
/// characters at, @p client joined and settled.
void JoinOnFloor(Session &session, Machine &host, Machine &client);

} // namespace Assisi::App::Testing
