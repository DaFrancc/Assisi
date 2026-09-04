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
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
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

TEST_CASE("Slices laid out end-to-end do not overlap once the trace has rounded them")
{
    EnsureInitialized();

    // The arithmetic a trace actually performs: it writes a begin and a
    // duration, each rounded to the nanosecond on its own, so a reader
    // reconstructs the end as the sum of two separately-rounded numbers rather
    // than from the end it was given. Packed flush, that sum lands past the next
    // slice's begin and an importer rejects every one of them.
    const auto toMicros = [](std::uint64_t ticks)
                          { return static_cast<double>(ticks) * 1e6 / Chiara::TicksPerSecond(); };
    const auto roundNs = [](double micros) { return std::round(micros * 1000.0) / 1000.0; };

    // Swept rather than hand-picked. Whether a particular pair of durations
    // rounds into an overlap depends on where both land against the nanosecond
    // grid, so a handful of chosen widths can miss it entirely while a real
    // capture — thousands of frames of arbitrary durations — hits it in one
    // slice out of six.
    std::vector<std::uint64_t> widths;
    widths.reserve(2000);
    std::uint64_t seed = 12345;
    for (std::int32_t i = 0; i < 2000; ++i)
    {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        widths.push_back((seed >> 33) % 4000000u);
    }

    // Begun far from zero, like a capture that has been running a while: the
    // microsecond values are then large enough that the two roundings have
    // somewhere to disagree.
    Chiara::TrackLayout layout(static_cast<std::uint64_t>(Chiara::TicksPerSecond() * 11.0));
    std::vector<std::pair<double, double>> placed;
    placed.reserve(widths.size());
    for (const std::uint64_t width : widths)
    {
        const std::uint64_t begin = layout.Place(width);
        const double reportedBegin = roundNs(toMicros(begin));
        const double reportedEnd = reportedBegin + roundNs(toMicros(begin + width) - toMicros(begin));
        placed.emplace_back(reportedBegin, reportedEnd);
    }

    for (std::size_t i = 1; i < placed.size(); ++i)
    {
        CAPTURE(i);
        CHECK(placed[i].first >= placed[i - 1].second);
    }
    // And the containing slice has to hold the last of them, or the parent
    // overlaps its own child.
    CHECK(roundNs(toMicros(layout.End())) >= placed.back().second);
}

TEST_CASE("The layout gap is never zero, whatever the clock")
{
    EnsureInitialized();

    // Two slices at the same tick are the same overlap by another route, and a
    // clock too coarse to express two nanoseconds still has to separate them.
    CHECK(Chiara::TrackLayout::GapTicks() >= 1);

    Chiara::TrackLayout layout(0);
    const std::uint64_t first = layout.Place(0);
    const std::uint64_t second = layout.Place(0);
    CHECK(second > first);
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
