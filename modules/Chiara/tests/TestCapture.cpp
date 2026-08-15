/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestCapture.cpp
/// @brief The runtime: scopes, nesting, args, counters, flows, frames, and the
///        shadow stack (design §3/§4).
///
/// Only meaningful in a Chiara-enabled build. In a default build the whole file
/// compiles to nothing, and TestMacros.cpp covers what remains.

#include "ChiaraTest.hpp"

#include <Assisi/Chiara/Profile.hpp>

#include <doctest/doctest.h>

#if defined(ASSISI_CHIARA_ENABLED)

#    include <bit>
#    include <cstring>
#    include <string>
#    include <thread>
#    include <vector>

using namespace Assisi;
using Assisi::ChiaraTest::EnsureInitialized;
using Assisi::ChiaraTest::MainCursor;
using Assisi::ChiaraTest::MainEventsSince;
using Assisi::ChiaraTest::MainSnapshot;
using Assisi::ChiaraTest::OfType;

TEST_CASE("A scope emits one record, stamped with its begin and duration")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    {
        ASSISI_PROFILE_SCOPE("solo");
    }

    const std::vector<Chiara::Event> scopes = OfType(MainEventsSince(mark), Chiara::EventType::Scope);
    REQUIRE(scopes.size() == 1);
    CHECK(std::strcmp(scopes[0].name, "solo") == 0);
    CHECK(scopes[0].timestampTicks > 0);
    // A scope written at destruction carries its own duration, so one record is
    // the whole slice — there is no matching end event to pair it with.
    CHECK(scopes[0].payload > 0);
}

TEST_CASE("Nested scopes come out inner-first and are contained by their parent")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    {
        ASSISI_PROFILE_SCOPE("outer");
        {
            ASSISI_PROFILE_SCOPE("inner");
        }
    }

    const std::vector<Chiara::Event> scopes = OfType(MainEventsSince(mark), Chiara::EventType::Scope);
    REQUIRE(scopes.size() == 2);

    // Records land in *end* order, which is why the child appears first.
    CHECK(std::strcmp(scopes[0].name, "inner") == 0);
    CHECK(std::strcmp(scopes[1].name, "outer") == 0);

    const Chiara::Event &inner = scopes[0];
    const Chiara::Event &outer = scopes[1];
    CHECK(inner.timestampTicks >= outer.timestampTicks);
    CHECK(inner.timestampTicks + inner.payload <= outer.timestampTicks + outer.payload);
}

TEST_CASE("Sibling scopes do not overlap")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    {
        ASSISI_PROFILE_SCOPE("first");
    }
    {
        ASSISI_PROFILE_SCOPE("second");
    }

    const std::vector<Chiara::Event> scopes = OfType(MainEventsSince(mark), Chiara::EventType::Scope);
    REQUIRE(scopes.size() == 2);
    CHECK(scopes[0].timestampTicks + scopes[0].payload <= scopes[1].timestampTicks);
}

TEST_CASE("Args are emitted inside the scope they belong to")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    {
        ASSISI_PROFILE_SCOPE("publish-mesh");
        ASSISI_PROFILE_ARG_STR("asset", "meshes/car_lod.gltf");
        ASSISI_PROFILE_ARG_U64("bytes", 4096u);
    }

    const std::vector<Chiara::Event> events = MainEventsSince(mark);
    REQUIRE(events.size() == 3);

    // Ordering is the whole contract here: args reach the ring before the scope
    // that owns them, because a scope is only written when it ends. The
    // serializer rebuilds the ownership from timestamps, so the args must fall
    // inside the scope's span.
    CHECK(events[0].type == Chiara::EventType::ArgString);
    CHECK(events[1].type == Chiara::EventType::ArgU64);
    CHECK(events[2].type == Chiara::EventType::Scope);

    CHECK(std::strcmp(events[0].name, "asset") == 0);
    const auto *value = std::bit_cast<const char *>(events[0].payload);
    CHECK(std::strcmp(value, "meshes/car_lod.gltf") == 0);

    CHECK(std::strcmp(events[1].name, "bytes") == 0);
    CHECK(events[1].payload == 4096u);

    const Chiara::Event &scope = events[2];
    CHECK(events[0].timestampTicks >= scope.timestampTicks);
    CHECK(events[1].timestampTicks <= scope.timestampTicks + scope.payload);
}

TEST_CASE("The scope name stays the aggregation key")
{
    // Guards the rule that context goes in args and never in the name: two
    // publishes of different assets must share one name, or cross-frame
    // aggregation shatters into singleton buckets.
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    for (const char *asset : {"a.gltf", "b.gltf"})
    {
        ASSISI_PROFILE_SCOPE("publish-mesh");
        ASSISI_PROFILE_ARG_STR("asset", asset);
    }

    const std::vector<Chiara::Event> scopes = OfType(MainEventsSince(mark), Chiara::EventType::Scope);
    REQUIRE(scopes.size() == 2);
    CHECK(scopes[0].name == scopes[1].name); // Same interned pointer, not merely equal text.
}

TEST_CASE("Counters round-trip their value")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    ASSISI_PROFILE_COUNTER("frame/cpu-ms", 12.5);
    ASSISI_PROFILE_COUNTER("frame/cpu-ms", -0.25);

    const std::vector<Chiara::Event> counters = OfType(MainEventsSince(mark), Chiara::EventType::Counter);
    REQUIRE(counters.size() == 2);
    CHECK(std::bit_cast<double>(counters[0].payload) == doctest::Approx(12.5));
    CHECK(std::bit_cast<double>(counters[1].payload) == doctest::Approx(-0.25));
}

TEST_CASE("Flow ids are unique, never zero, and survive the round trip")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    const std::uint64_t first  = Chiara::NewFlowId();
    const std::uint64_t second = Chiara::NewFlowId();
    CHECK(first != 0);
    CHECK(second != first);

    ASSISI_PROFILE_FLOW_BEGIN("staging-lifetime", first);
    ASSISI_PROFILE_FLOW_END("staging-lifetime", first);

    const std::vector<Chiara::Event> events = MainEventsSince(mark);
    const std::vector<Chiara::Event> begins = OfType(events, Chiara::EventType::FlowBegin);
    const std::vector<Chiara::Event> ends   = OfType(events, Chiara::EventType::FlowEnd);
    REQUIRE(begins.size() == 1);
    REQUIRE(ends.size() == 1);
    CHECK(begins[0].payload == first);
    CHECK(ends[0].payload == first);
}

TEST_CASE("Frame marks carry a monotonically advancing index")
{
    EnsureInitialized();
    const std::uint64_t mark   = MainCursor();
    const std::uint64_t before = Chiara::CurrentFrame();

    ASSISI_PROFILE_FRAME();
    ASSISI_PROFILE_FRAME();

    CHECK(Chiara::CurrentFrame() == before + 2);

    const std::vector<Chiara::Event> frames = OfType(MainEventsSince(mark), Chiara::EventType::FrameMark);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].payload == before + 1);
    CHECK(frames[1].payload == before + 2);
}

TEST_CASE("Async spans pair across threads")
{
    // The reason they exist: a job continuation finishes on a thread that never
    // saw the start, so forcing this into a scope stack would be a lie.
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    const std::uint64_t asyncId = Chiara::BeginAsync("stream-load");
    CHECK(asyncId != 0);

    std::thread finisher([asyncId] { Chiara::EndAsync("stream-load", asyncId); });
    finisher.join();

    const std::vector<Chiara::Event> begins = OfType(MainEventsSince(mark), Chiara::EventType::AsyncBegin);
    REQUIRE(begins.size() == 1);
    CHECK(begins[0].payload == asyncId);

    // The end landed on the other thread's ring, which is exactly the point.
    bool foundEnd = false;
    for (const Chiara::ThreadSnapshot &snapshot : Chiara::SnapshotThreads())
    {
        if (snapshot.isMain || snapshot.ring == nullptr)
        {
            continue;
        }
        for (std::uint64_t index = snapshot.beginIndex; index < snapshot.endIndex; ++index)
        {
            const Chiara::Event &event = snapshot.ring->At(index);
            if (event.type == Chiara::EventType::AsyncEnd && event.payload == asyncId)
            {
                foundEnd = true;
            }
        }
    }
    CHECK(foundEnd);
}

TEST_CASE("A paused capture records nothing")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    Chiara::SetRecording(false);
    CHECK_FALSE(Chiara::IsRecording());
    {
        ASSISI_PROFILE_SCOPE("ignored");
        ASSISI_PROFILE_COUNTER("ignored", 1.0);
        ASSISI_PROFILE_ARG_U64("ignored", 1u);
    }
    Chiara::SetRecording(true);

    CHECK(MainEventsSince(mark).empty());
}

TEST_CASE("A scope that ends while paused is dropped, not half-written")
{
    // Emitting it anyway would put a second record past the cursor a reader
    // already sampled, breaking the at-most-one-straggler bound the read
    // protocol depends on.
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    {
        ASSISI_PROFILE_SCOPE("spans-the-pause");
        Chiara::SetRecording(false);
    }
    Chiara::SetRecording(true);

    CHECK(OfType(MainEventsSince(mark), Chiara::EventType::Scope).empty());
}

TEST_CASE("Open scopes are visible while they are still running")
{
    // The hang case: a scope only reaches the ring when it ends, so without the
    // shadow stack a capture taken mid-hang shows nothing for the hanging scope.
    EnsureInitialized();

    {
        ASSISI_PROFILE_SCOPE("still-open-outer");
        {
            ASSISI_PROFILE_SCOPE("still-open-inner");

            const Chiara::ThreadSnapshot snapshot = MainSnapshot();
            REQUIRE(snapshot.openScopes.size() == 2);
            CHECK(std::strcmp(snapshot.openScopes[0].name, "still-open-outer") == 0);
            CHECK(std::strcmp(snapshot.openScopes[1].name, "still-open-inner") == 0);
            CHECK(snapshot.openScopes[1].beginTicks >= snapshot.openScopes[0].beginTicks);
        }
    }

    CHECK(MainSnapshot().openScopes.empty());
}

TEST_CASE("Interning returns one stable pointer per distinct string")
{
    const char *first  = Chiara::InternString("meshes/car_lod.gltf");
    const char *second = Chiara::InternString(std::string("meshes/car_lod.gltf"));
    const char *other  = Chiara::InternString("meshes/other.gltf");

    CHECK(first == second);
    CHECK(first != other);
    CHECK(std::strcmp(first, "meshes/car_lod.gltf") == 0);
}

TEST_CASE("The clock is calibrated and snapshots pair it with the reference clock")
{
    EnsureInitialized();
    const std::uint64_t mark = MainCursor();

    CHECK(Chiara::TicksPerSecond() > 1'000'000.0); // Anything plausible: ns fallback is 1e9, a TSC is ~1e9-5e9.
    CHECK(Chiara::ReadTicks() > 0);

    Chiara::EmitClockSnapshot();

    const std::vector<Chiara::Event> snapshots = OfType(MainEventsSince(mark), Chiara::EventType::ClockSnapshot);
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].payload > 0); // CLOCK_MONOTONIC_RAW nanoseconds.
    CHECK(snapshots[0].timestampTicks > 0);
}

TEST_CASE("The main thread is registered and named")
{
    EnsureInitialized();
    const Chiara::ThreadSnapshot snapshot = MainSnapshot();
    REQUIRE(snapshot.ring != nullptr);
    CHECK(std::strcmp(snapshot.name, "main") == 0);
    CHECK(snapshot.isMain);
}

TEST_CASE("An unregistered thread registers itself on its first record")
{
    EnsureInitialized();

    std::thread anonymous(
        []
        {
        ASSISI_PROFILE_SCOPE("from-an-unnamed-thread");
        });
    anonymous.join();

    bool foundAutoNamed = false;
    for (const Chiara::ThreadSnapshot &snapshot : Chiara::SnapshotThreads())
    {
        if (snapshot.name != nullptr && std::string(snapshot.name).starts_with("thread-"))
        {
            foundAutoNamed = true;
        }
    }
    CHECK(foundAutoNamed);
}

#endif // ASSISI_CHIARA_ENABLED
