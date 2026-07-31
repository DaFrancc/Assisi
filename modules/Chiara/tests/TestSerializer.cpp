/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestSerializer.cpp
/// @brief Chrome JSON export (design §6).
///
/// Every case emits a known scene, serializes it, and parses the result back
/// with nlohmann — asserting on the *parsed* trace rather than on text, so the
/// tests pin the shape a viewer will see rather than the formatting.

#include "ChiaraTest.hpp"

#include <Assisi/Chiara/Profile.hpp>
#include <Assisi/Chiara/Serializer.hpp>

#include <doctest/doctest.h>

#if defined(ASSISI_CHIARA_ENABLED)

#    include <nlohmann/json.hpp>

#    include <cstdint>
#    include <filesystem>
#    include <fstream>
#    include <string>
#    include <thread>
#    include <vector>

using namespace Assisi;
using Assisi::ChiaraTest::EnsureInitialized;
using nlohmann::json;

namespace
{

/// @brief A capture file that cleans up after itself.
class TempTrace
{
public:
    explicit TempTrace(const char *label)
        : _path(std::filesystem::temp_directory_path() / (std::string("chiara-test-") + label + ".json"))
    {
        std::filesystem::remove(_path);
    }

    ~TempTrace() { std::filesystem::remove(_path); }

    TempTrace(const TempTrace &)            = delete;
    TempTrace &operator=(const TempTrace &) = delete;
    TempTrace(TempTrace &&)                 = delete;
    TempTrace &operator=(TempTrace &&)      = delete;

    [[nodiscard]] const std::filesystem::path &Path() const { return _path; }

    [[nodiscard]] json Parse() const
    {
        std::ifstream input(_path);
        REQUIRE(input.is_open());
        json parsed;
        input >> parsed;
        return parsed;
    }

private:
    std::filesystem::path _path;
};

/// @brief Every trace event of one phase.
[[nodiscard]] std::vector<json> EventsOfPhase(const json &trace, std::string_view phase)
{
    std::vector<json> matching;
    for (const json &event : trace.at("traceEvents"))
    {
        if (event.at("ph") == phase)
        {
            matching.push_back(event);
        }
    }
    return matching;
}

/// @brief The first event of a phase with a given name, or a null json.
[[nodiscard]] json FindNamed(const json &trace, std::string_view phase, std::string_view name)
{
    for (const json &event : EventsOfPhase(trace, phase))
    {
        if (event.contains("name") && event.at("name") == name)
        {
            return event;
        }
    }
    return json{};
}

} // namespace

TEST_CASE("A capture serializes to a trace a viewer can parse")
{
    EnsureInitialized();
    const TempTrace trace("basic");

    {
        ASSISI_PROFILE_SCOPE("serialize-outer");
        {
            ASSISI_PROFILE_SCOPE("serialize-inner");
        }
    }

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());
    REQUIRE(result.error.empty());
    REQUIRE(result.success);
    CHECK(result.eventsWritten > 0);
    CHECK(result.bytesWritten > 0);
    CHECK(result.threadsWritten >= 1);
    CHECK(Chiara::IsRecording()); // Recording is restored, not left paused.

    const json parsed = trace.Parse();
    CHECK(parsed.at("displayTimeUnit") == "ms");
    REQUIRE(parsed.contains("traceEvents"));

    const json outer = FindNamed(parsed, "X", "serialize-outer");
    const json inner = FindNamed(parsed, "X", "serialize-inner");
    REQUIRE_FALSE(outer.is_null());
    REQUIRE_FALSE(inner.is_null());

    // Complete events: begin plus duration, in fractional microseconds.
    CHECK(outer.at("dur").get<double>() > 0.0);
    const double outerBegin = outer.at("ts").get<double>();
    const double innerBegin = inner.at("ts").get<double>();
    CHECK(innerBegin >= outerBegin);
    CHECK(innerBegin + inner.at("dur").get<double>() <= outerBegin + outer.at("dur").get<double>());
}

TEST_CASE("Threads are named and the main one sorts first")
{
    EnsureInitialized();
    const TempTrace trace("metadata");

    {
        ASSISI_PROFILE_SCOPE("metadata-scope");
    }

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());
    REQUIRE(result.success);

    const json parsed   = trace.Parse();
    const json metadata = FindNamed(parsed, "M", "process_name");
    REQUIRE_FALSE(metadata.is_null());
    CHECK(metadata.at("args").at("name") == "Assisi");

    bool foundMainThread = false;
    for (const json &event : EventsOfPhase(parsed, "M"))
    {
        if (event.at("name") == "thread_name" && event.at("args").at("name") == "main")
        {
            foundMainThread = true;
            const std::int32_t mainTid = event.at("tid").get<std::int32_t>();

            bool foundSortIndex = false;
            for (const json &other : EventsOfPhase(parsed, "M"))
            {
                if (other.at("name") == "thread_sort_index" && other.at("tid").get<std::int32_t>() == mainTid)
                {
                    CHECK(other.at("args").at("sort_index").get<std::int32_t>() == 0);
                    foundSortIndex = true;
                }
            }
            CHECK(foundSortIndex);
        }
    }
    CHECK(foundMainThread);
}

TEST_CASE("Args are folded into the slice that owns them")
{
    // The serializer's real work: args reach the ring *before* the scope that
    // encloses them, so ownership has to be rebuilt from timestamps.
    EnsureInitialized();
    const TempTrace trace("args");

    {
        ASSISI_PROFILE_SCOPE("arg-owner");
        ASSISI_PROFILE_ARG_STR("asset", "meshes/car_lod.gltf");
        ASSISI_PROFILE_ARG_U64("bytes", 4096u);
        {
            ASSISI_PROFILE_SCOPE("arg-child");
            ASSISI_PROFILE_ARG_STR("asset", "textures/paint.png");
        }
    }

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());
    REQUIRE(result.success);
    CHECK(result.orphanedArgs == 0);

    const json parsed = trace.Parse();
    const json owner  = FindNamed(parsed, "X", "arg-owner");
    const json child  = FindNamed(parsed, "X", "arg-child");
    REQUIRE_FALSE(owner.is_null());
    REQUIRE_FALSE(child.is_null());

    // Innermost wins: the child's arg must not have been claimed by the parent.
    REQUIRE(owner.contains("args"));
    CHECK(owner.at("args").at("asset") == "meshes/car_lod.gltf");
    CHECK(owner.at("args").at("bytes").get<std::uint64_t>() == 4096u);

    REQUIRE(child.contains("args"));
    CHECK(child.at("args").at("asset") == "textures/paint.png");
}

TEST_CASE("An arg emitted outside any scope is dropped and counted")
{
    // Never silently: a vanishing arg is indistinguishable from one that was
    // never emitted, and that is exactly the bug this would hide.
    EnsureInitialized();
    const TempTrace trace("orphan");

    ASSISI_PROFILE_ARG_STR("stray", "no-enclosing-scope");

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path(), 0.0);
    REQUIRE(result.success);
    CHECK(result.orphanedArgs >= 1);
}

TEST_CASE("A scope still running at dump time is drawn and flagged")
{
    // The hang case. A scope only reaches the ring when it ends, so the one that
    // is hanging is precisely the one a naive capture would omit.
    EnsureInitialized();
    const TempTrace trace("still-open");

    json parsed;
    {
        ASSISI_PROFILE_SCOPE("hanging-scope");

        const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());
        REQUIRE(result.success);
        CHECK(result.slicesSynthesized >= 1);
        parsed = trace.Parse();
    }

    const json hanging = FindNamed(parsed, "X", "hanging-scope");
    REQUIRE_FALSE(hanging.is_null());
    CHECK(hanging.at("dur").get<double>() >= 0.0);
    REQUIRE(hanging.contains("args"));
    CHECK(hanging.at("args").at("chiara.still_open") == true);
}

TEST_CASE("Counters, flows, async spans and frames map to their trace phases")
{
    EnsureInitialized();
    const TempTrace trace("phases");

    // Names are unique to this case on purpose. A dump with no window holds
    // every earlier case's records too, so a shared name would resolve to
    // whichever ran first and the assertion would be testing that instead.
    const std::uint64_t flowId = Chiara::NewFlowId();
    {
        ASSISI_PROFILE_SCOPE("phase-carrier");
        ASSISI_PROFILE_COUNTER("phase/cpu-ms", 12.5);
        ASSISI_PROFILE_FLOW_BEGIN("phase-flow", flowId);
        ASSISI_PROFILE_FLOW_END("phase-flow", flowId);
        ASSISI_PROFILE_FRAME();
    }
    const std::uint64_t asyncId = Chiara::BeginAsync("phase-async");
    Chiara::EndAsync("phase-async", asyncId);

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());
    REQUIRE(result.success);

    const json parsed = trace.Parse();

    const json counter = FindNamed(parsed, "C", "phase/cpu-ms");
    REQUIRE_FALSE(counter.is_null());
    CHECK(counter.at("args").at("v").get<double>() == doctest::Approx(12.5));

    const json flowBegin = FindNamed(parsed, "s", "phase-flow");
    const json flowEnd   = FindNamed(parsed, "f", "phase-flow");
    REQUIRE_FALSE(flowBegin.is_null());
    REQUIRE_FALSE(flowEnd.is_null());
    CHECK(flowBegin.at("id").get<std::uint64_t>() == flowId);
    CHECK(flowEnd.at("id").get<std::uint64_t>() == flowId);
    // Without bp:"e" the arrow binds to the next slice to start rather than the
    // one that actually paid the cost.
    CHECK(flowEnd.at("bp") == "e");

    const json asyncBegin = FindNamed(parsed, "b", "phase-async");
    const json asyncEnd   = FindNamed(parsed, "e", "phase-async");
    REQUIRE_FALSE(asyncBegin.is_null());
    REQUIRE_FALSE(asyncEnd.is_null());
    // Chrome JSON pairs async events by (cat, id) — a missing cat means nothing
    // pairs, which looks like a working trace right up until you open it.
    CHECK(asyncBegin.at("cat") == "async");
    CHECK(asyncEnd.at("cat") == "async");
    CHECK(asyncBegin.at("id").get<std::uint64_t>() == asyncId);

    CHECK_FALSE(FindNamed(parsed, "i", "frame").is_null());
    CHECK_FALSE(FindNamed(parsed, "C", "frame").is_null());
}

TEST_CASE("Clock snapshots never appear as slices")
{
    // They are calibration data, not something to look at.
    EnsureInitialized();
    const TempTrace trace("clock");

    Chiara::EmitClockSnapshot();
    {
        ASSISI_PROFILE_SCOPE("clock-carrier");
    }

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());
    REQUIRE(result.success);
    CHECK(result.ticksPerSecond > 1'000'000.0);

    const json parsed = trace.Parse();
    CHECK(FindNamed(parsed, "X", "clock-snapshot").is_null());
    CHECK(FindNamed(parsed, "i", "clock-snapshot").is_null());
}

TEST_CASE("A window trims older work and keeps what straddles its edge")
{
    EnsureInitialized();
    const TempTrace trace("window");

    {
        ASSISI_PROFILE_SCOPE("ancient");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    {
        ASSISI_PROFILE_SCOPE("recent");
    }

    // A window far shorter than the gap above must keep only the second scope.
    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path(), 0.010);
    REQUIRE(result.success);
    CHECK(result.windowSeconds <= 0.05);

    const json parsed = trace.Parse();
    CHECK_FALSE(FindNamed(parsed, "X", "recent").is_null());
    CHECK(FindNamed(parsed, "X", "ancient").is_null());
}

TEST_CASE("Names with JSON metacharacters survive the round trip")
{
    EnsureInitialized();
    const TempTrace trace("escaping");

    {
        ASSISI_PROFILE_SCOPE("escaping-slice");
        ASSISI_PROFILE_ARG_STR("asset", "meshes\\odd \"name\"\n.gltf");
    }

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());
    REQUIRE(result.success);

    const json parsed = trace.Parse(); // Would throw on malformed escaping.
    const json slice  = FindNamed(parsed, "X", "escaping-slice");
    REQUIRE_FALSE(slice.is_null());
    CHECK(slice.at("args").at("asset") == "meshes\\odd \"name\"\n.gltf");
}

TEST_CASE("Serializing while other threads emit stays consistent")
{
    // Stage 6 runs the dump on a JobSystem worker while the frame keeps going,
    // so the pause-drain-read protocol has to hold with producers live.
    EnsureInitialized();
    const TempTrace trace("concurrent");

    std::atomic<bool>        keepEmitting{true};
    std::vector<std::thread> emitters;
    emitters.reserve(4);
    for (std::int32_t index = 0; index < 4; ++index)
    {
        emitters.emplace_back(
            [&keepEmitting]
            {
                Chiara::RegisterCurrentThread("chiara-emit");
                while (keepEmitting.load(std::memory_order_relaxed))
                {
                    ASSISI_PROFILE_SCOPE("concurrent-work");
                    ASSISI_PROFILE_COUNTER("concurrent/value", 1.0);
                }
            });
    }

    const Chiara::SerializeResult result = Chiara::SerializeCapture(trace.Path());

    keepEmitting.store(false, std::memory_order_relaxed);
    for (std::thread &emitter : emitters)
    {
        emitter.join();
    }

    REQUIRE(result.error.empty());
    REQUIRE(result.success);
    CHECK(Chiara::IsRecording());

    const json parsed = trace.Parse(); // Parsing at all is the assertion.
    CHECK(parsed.at("traceEvents").size() > 0);
}

#endif // ASSISI_CHIARA_ENABLED
