/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFrameEvents.cpp
/// @brief No phase reads an event twice, at any frame rate: what each phase of
///        a frame sees of what was pushed earlier in it, in its ticks, and in
///        the frames before it.
///
/// Each frame is run as the game runs one, through the functions it calls:
/// BeginWorldFrame, StepWorld once a tick, UpdateWorld, PresentWorld and
/// EndWorldFrame, with the universal queue marked and ended where Application
/// does it.

#include <doctest/doctest.h>

#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace Assisi::App;
namespace Core = Assisi::Core;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// A world event, as most events are.
struct Ping
{
};

/// How many events a reader saw, once for each time its phase ran.
using Reads = std::vector<std::size_t>;

template <typename E>
void PushIfArmed(SystemContext &ctx, bool &armed)
{
    if (armed)
    {
        armed = false;
        ctx.events.Push(E{});
    }
}

/// Pushes one E from @p phase the next time it runs with @p armed set; first in
/// the phase, so the rest of it can read it.
template <typename E>
void PushWhenArmed(World &world, SystemPhase phase, const char *name, bool &armed)
{
    world.systems.Register(phase, name, [&armed](SystemContext &ctx) { PushIfArmed<E>(ctx, armed); }).First();
}

/// Notes in @p reads how many E's @p phase sees each time it runs; last in the
/// phase, so it sees what the rest of it pushed.
template <typename E>
void CountReads(World &world, SystemPhase phase, const char *name, Reads &reads)
{
    world.systems.Register(phase, name, [&reads](SystemContext &ctx) { reads.push_back(ctx.events.Read<E>().size()); })
    .Last();
}

/// The worlds of a game and the universal queue they share, run a frame at a
/// time.
struct Frames
{
    Core::EventQueue universal{Core::EventScope::Universal};
    WorldManager worlds{Assisi::Physics::NoCollisionAssets()};
    World &world = worlds.Create("Frames");
    /// The worlds each frame runs, in the order it runs them.
    std::vector<World *> running{&world};

    SystemContext Context(World &in)
    {
        return {.world = in,
                .dt = kStep,
                .simTick = 0,
                .input = nullptr,
                .actions = nullptr,
                .universalEvents = universal,
                .isActiveWorld = true,
                .worldManager = &worlds};
    }

    /// One frame of @p ticks fixed steps. A paused world runs its frame's phases
    /// and none of its steps, as the game runs it.
    void Frame(std::uint32_t ticks)
    {
        for (World *const in : running)
        {
            BeginWorldFrame(Context(*in));
        }
        universal.MarkTicks();
        for (std::uint32_t tick = 0; tick < ticks; ++tick)
        {
            for (World *const in : running)
            {
                if (!in->paused)
                {
                    StepWorld(Context(*in));
                }
            }
        }
        for (World *const in : running)
        {
            UpdateWorld(Context(*in));
            PresentWorld(Context(*in));
        }
        universal.EndFrame(/*keepPending=*/ ticks == 0);
        for (World *const in : running)
        {
            EndWorldFrame(*in, universal, /*ticking=*/ !in->paused);
        }
    }
};

} // namespace

TEST_CASE("Two ticks in a frame read an event the first pushed once between them, and Update reads it once")
{
    Frames frames;
    bool armed = true;
    PushWhenArmed<Ping>(frames.world, SystemPhase::FixedUpdate, "Push", armed);
    Reads fixed, postFixed, update, ui;
    CountReads<Ping>(frames.world, SystemPhase::FixedUpdate, "ReadFixed", fixed);
    CountReads<Ping>(frames.world, SystemPhase::PostFixedUpdate, "ReadPostFixed", postFixed);
    CountReads<Ping>(frames.world, SystemPhase::Update, "ReadUpdate", update);
    CountReads<Ping>(frames.world, SystemPhase::Ui, "ReadUi", ui);

    frames.Frame(2);

    CHECK(fixed == Reads{1, 0});
    CHECK(postFixed == Reads{1, 0});
    CHECK(update == Reads{1});
    CHECK(ui == Reads{1});
}

TEST_CASE("A PreUpdate event reaches the frame's first tick and no other")
{
    Frames frames;
    bool armed = true;
    PushWhenArmed<Ping>(frames.world, SystemPhase::PreUpdate, "Push", armed);
    Reads fixed, update;
    CountReads<Ping>(frames.world, SystemPhase::FixedUpdate, "ReadFixed", fixed);
    CountReads<Ping>(frames.world, SystemPhase::Update, "ReadUpdate", update);

    frames.Frame(2);

    CHECK(fixed == Reads{1, 0});
    CHECK(update == Reads{1});
}

TEST_CASE("An Update event never reaches a tick, on a frame with ticks or without")
{
    Frames frames;
    bool armed = true;
    PushWhenArmed<Ping>(frames.world, SystemPhase::Update, "Push", armed);
    Reads fixed, ui;
    CountReads<Ping>(frames.world, SystemPhase::FixedUpdate, "ReadFixed", fixed);
    CountReads<Ping>(frames.world, SystemPhase::Ui, "ReadUi", ui);

    frames.Frame(1);
    armed = true;
    frames.Frame(0);
    frames.Frame(1);

    CHECK(fixed == Reads{0, 0});
    CHECK(ui == Reads{1, 1, 0});
}

TEST_CASE("On frames that run no tick, a click and a PreUpdate event wait for the next tick, which reads them once")
{
    Frames frames;
    bool armed = true;
    PushWhenArmed<Ping>(frames.world, SystemPhase::PreUpdate, "Push", armed);
    Reads preUpdate, fixed, update;
    CountReads<Ping>(frames.world, SystemPhase::PreUpdate, "ReadPreUpdate", preUpdate);
    CountReads<Ping>(frames.world, SystemPhase::FixedUpdate, "ReadFixed", fixed);
    CountReads<Ping>(frames.world, SystemPhase::Update, "ReadUpdate", update);

    // Where the UI's input step pushes a click: before the frame's systems.
    frames.world.events.Push(Ping{});
    frames.Frame(0);
    frames.Frame(0);
    frames.Frame(1);
    frames.Frame(1);

    CHECK(preUpdate == Reads{2, 0, 0, 0});
    CHECK(fixed == Reads{2, 0});
    CHECK(update == Reads{2, 0, 0, 0});
}

TEST_CASE("A paused world keeps nothing for its next tick")
{
    Frames frames;
    bool armed = true;
    PushWhenArmed<Ping>(frames.world, SystemPhase::PreUpdate, "Push", armed);
    Reads fixed;
    CountReads<Ping>(frames.world, SystemPhase::FixedUpdate, "ReadFixed", fixed);

    frames.world.events.Push(Ping{});
    frames.world.paused = true;
    frames.Frame(0);
    frames.world.paused = false;
    frames.Frame(1);

    CHECK(fixed == Reads{0});
}

TEST_CASE("A paused world lets the universal events waiting for a tick go by, and a ticking one reads them")
{
    Frames frames;
    World &other = frames.worlds.Create("Other");
    frames.running.push_back(&other);
    bool armed = true;
    PushWhenArmed<QuitRequested>(frames.world, SystemPhase::PreUpdate, "Push", armed);
    Reads ticking, paused;
    CountReads<QuitRequested>(frames.world, SystemPhase::FixedUpdate, "ReadFixed", ticking);
    CountReads<QuitRequested>(other, SystemPhase::FixedUpdate, "ReadFixed", paused);

    other.paused = true;
    frames.Frame(0);
    other.paused = false;
    frames.Frame(1);

    CHECK(ticking == Reads{1});
    CHECK(paused == Reads{0});
}

TEST_CASE("A universal event one world's tick pushes is read once by each world's ticks")
{
    Frames frames;
    World &other = frames.worlds.Create("Other");
    frames.running.push_back(&other);
    bool armed = true;
    PushWhenArmed<QuitRequested>(frames.world, SystemPhase::FixedUpdate, "Push", armed);
    Reads pusherTicks, otherTicks, pusherUpdate, otherUpdate;
    CountReads<QuitRequested>(frames.world, SystemPhase::PostFixedUpdate, "ReadPostFixed", pusherTicks);
    CountReads<QuitRequested>(other, SystemPhase::PostFixedUpdate, "ReadPostFixed", otherTicks);
    CountReads<QuitRequested>(frames.world, SystemPhase::Update, "ReadUpdate", pusherUpdate);
    CountReads<QuitRequested>(other, SystemPhase::Update, "ReadUpdate", otherUpdate);

    frames.Frame(2);

    CHECK(pusherTicks == Reads{1, 0});
    CHECK(otherTicks == Reads{1, 0});
    CHECK(pusherUpdate == Reads{1});
    CHECK(otherUpdate == Reads{1});
}

TEST_CASE("A tick a client steps again moves no mark, so what was pushed before the ticks reaches the live one")
{
    Frames frames;
    bool armed = true;
    PushWhenArmed<Ping>(frames.world, SystemPhase::PreUpdate, "Push", armed);
    Reads fixed;
    CountReads<Ping>(frames.world, SystemPhase::FixedUpdate, "ReadFixed", fixed);

    BeginWorldFrame(frames.Context(frames.world));
    frames.universal.MarkTicks();
    SystemContext replayed = frames.Context(frames.world);
    replayed.replaying = true;
    StepWorld(replayed);
    StepWorld(frames.Context(frames.world));

    CHECK(fixed == Reads{1});
}
