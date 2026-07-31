/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestPreInit.cpp
/// @brief Chiara before anyone calls Initialize.
///
/// Its own executable on purpose: the property under test is "nothing has
/// started yet", which cannot survive sharing a process with cases that start
/// the capture, and doctest makes no promise about ordering. Nothing in this
/// binary may ever call Chiara::Initialize.
///
/// This matters beyond tidiness. Core's JobSystem runs in headless tests and
/// tools that have no Application and therefore no InitGuard, so its worker
/// threads call RegisterCurrentThread with the runtime down. A Chiara-enabled
/// build of those has to behave exactly like a default build: no crash, no
/// implicit start, no records.

#include <Assisi/Chiara/Profile.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <thread>

using namespace Assisi;

TEST_CASE("Nothing records before Initialize")
{
    CHECK_FALSE(Chiara::IsRecording());

    {
        ASSISI_PROFILE_SCOPE("before-init");
        ASSISI_PROFILE_ARG_STR("asset", "unused.gltf");
        ASSISI_PROFILE_ARG_U64("bytes", 1u);
        ASSISI_PROFILE_COUNTER("test/value", 1.0);
        ASSISI_PROFILE_FLOW_BEGIN("flow", 1u);
        ASSISI_PROFILE_FLOW_END("flow", 1u);
        ASSISI_PROFILE_FRAME();
    }

    const Chiara::CaptureStats stats = Chiara::GetCaptureStats();
    CHECK(stats.totalEventsWritten == 0);
    CHECK(stats.threadCount == 0);
    CHECK(Chiara::SnapshotThreads().empty());
}

TEST_CASE("Naming a thread before Initialize is inert, not an implicit start")
{
    Chiara::RegisterCurrentThread("premature");

    std::thread worker([] { Chiara::RegisterCurrentThread("premature-worker"); });
    worker.join();

    CHECK_FALSE(Chiara::IsRecording());
    CHECK(Chiara::SnapshotThreads().empty());
}

TEST_CASE("Recording cannot be switched on before Initialize")
{
    Chiara::SetRecording(true);
    CHECK_FALSE(Chiara::IsRecording());

    ASSISI_PROFILE_COUNTER("test/value", 1.0);
    CHECK(Chiara::GetCaptureStats().totalEventsWritten == 0);
}

TEST_CASE("Async spans and flow ids stay usable with the runtime down")
{
    // Callers store these handles in engine state; handing back something
    // unusable would push an #ifdef into every call site.
    const std::uint64_t asyncId = Chiara::BeginAsync("stream-load");
    Chiara::EndAsync("stream-load", asyncId);

    CHECK(Chiara::NewFlowId() != 0);
    CHECK(Chiara::GetCaptureStats().totalEventsWritten == 0);
}
