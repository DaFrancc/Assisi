/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestTracks.cpp
/// @brief Rows that are not threads, and slices measured somewhere else.
///
/// A track's whole job is to carry work that no call stack was holding when it
/// happened — the GPU's passes, whose durations arrive frames after the fact. So
/// what has to hold is that the times are the caller's and not the clock's, and
/// that a track never passes for a thread.

#include "ChiaraTest.hpp"

#include <Assisi/Chiara/Profile.hpp>

#include <doctest/doctest.h>

#if defined(ASSISI_CHIARA_ENABLED)

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace Assisi;
using Assisi::ChiaraTest::EnsureInitialized;

namespace
{
/// The snapshot of the track named @p name, or a default one.
Chiara::ThreadSnapshot TrackSnapshot(std::string_view name)
{
    for (Chiara::ThreadSnapshot &snapshot : Chiara::SnapshotThreads())
    {
        if (snapshot.name != nullptr && std::string_view(snapshot.name) == name)
        {
            return snapshot;
        }
    }
    return {};
}

std::vector<Chiara::Event> EventsOf(const Chiara::ThreadSnapshot &snapshot)
{
    std::vector<Chiara::Event> events;
    if (snapshot.ring == nullptr)
    {
        return events;
    }
    for (std::uint64_t index = snapshot.beginIndex; index < snapshot.endIndex; ++index)
    {
        events.push_back(snapshot.ring->At(index));
    }
    return events;
}
} // namespace

TEST_CASE("A track records the times it was given, not the time it was told")
{
    EnsureInitialized();

    Chiara::Track *track = Chiara::RegisterTrack("test-track-times");
    REQUIRE(track != nullptr);

    // Deliberately nowhere near now. The whole point of a track is work measured
    // elsewhere, so an emit that stamped the clock would describe the moment of
    // reporting rather than the work — which for a GPU pass is a fixed lie two
    // frames wide.
    constexpr std::uint64_t kBegin = 1000;
    constexpr std::uint64_t kDuration = 250;
    Chiara::EmitScopeOn(track, "pass", kBegin, kDuration);

    const std::vector<Chiara::Event> events = EventsOf(TrackSnapshot("test-track-times"));
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == Chiara::EventType::Scope);
    CHECK(events[0].timestampTicks == kBegin);
    CHECK(events[0].payload == kDuration);
    CHECK(std::string_view(events[0].name) == "pass");
}

TEST_CASE("A track is never the main thread")
{
    EnsureInitialized();

    Chiara::Track *track = Chiara::RegisterTrack("test-track-not-main");
    REQUIRE(track != nullptr);

    // isMain sorts a row to the top and names it the process's own. A track that
    // won that by registering early would put the GPU above the thread it is
    // meant to be read beside.
    const Chiara::ThreadSnapshot snapshot = TrackSnapshot("test-track-not-main");
    CHECK_FALSE(snapshot.isMain);
    CHECK(ChiaraTest::MainSnapshot().ring != snapshot.ring);
}

TEST_CASE("A track does not become the emitting thread's own buffer")
{
    EnsureInitialized();

    // The defect this guards: registering a track through the thread path would
    // hand the caller's own scopes to the track's ring from then on, and the
    // thread's slices would silently move rows.
    const std::uint64_t mark = ChiaraTest::MainCursor();
    Chiara::Track *track = Chiara::RegisterTrack("test-track-isolation");
    REQUIRE(track != nullptr);

    {
        ASSISI_PROFILE_SCOPE("on-the-thread");
    }
    Chiara::EmitScopeOn(track, "on-the-track", 1, 1);

    const std::vector<Chiara::Event> mine =
        ChiaraTest::OfType(ChiaraTest::MainEventsSince(mark), Chiara::EventType::Scope);
    CHECK(std::any_of(mine.begin(), mine.end(),
                      [](const Chiara::Event &e) { return std::string_view(e.name) == "on-the-thread"; }));
    CHECK_FALSE(std::any_of(mine.begin(), mine.end(),
                            [](const Chiara::Event &e) { return std::string_view(e.name) == "on-the-track"; }));

    const std::vector<Chiara::Event> theirs = EventsOf(TrackSnapshot("test-track-isolation"));
    REQUIRE(theirs.size() == 1);
    CHECK(std::string_view(theirs[0].name) == "on-the-track");
}

TEST_CASE("Emitting on a null track is a no-op rather than a crash")
{
    EnsureInitialized();

    // RegisterTrack returns null before Initialize, and every caller keeps the
    // pointer rather than re-asking. Making that safe is what stops a profiler
    // from being the thing that takes the process down.
    Chiara::EmitScopeOn(nullptr, "nowhere", 1, 1);
}

#endif // ASSISI_CHIARA_ENABLED
