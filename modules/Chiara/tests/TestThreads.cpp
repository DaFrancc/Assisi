/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestThreads.cpp
/// @brief Many producers at once, with a reader walking their live shadow
///        stacks the whole time. This is the case the tsan target exists for.

#include "ChiaraTest.hpp"

#include <Assisi/Chiara/Profile.hpp>

#include <doctest/doctest.h>

#if defined(ASSISI_CHIARA_ENABLED)

#    include <atomic>
#    include <cstring>
#    include <string>
#    include <thread>
#    include <vector>

using namespace Assisi;
using Assisi::ChiaraTest::EnsureInitialized;

TEST_CASE("Eight threads emit concurrently while a reader walks their open scopes")
{
    EnsureInitialized();

    constexpr std::int32_t kThreads    = 8;
    constexpr std::int32_t kIterations = 300;

    std::atomic<std::int32_t> ready{0};
    std::atomic<bool>         keepReading{true};
    std::vector<std::thread>  workers;
    workers.reserve(kThreads);

    for (std::int32_t index = 0; index < kThreads; ++index)
    {
        workers.emplace_back(
            [index, &ready]
            {
                Chiara::RegisterCurrentThread(("chiara-w" + std::to_string(index)).c_str());
                ready.fetch_add(1, std::memory_order_release);

                for (std::int32_t iteration = 0; iteration < kIterations; ++iteration)
                {
                    ASSISI_PROFILE_SCOPE("worker-outer");
                    ASSISI_PROFILE_ARG_U64("iteration", static_cast<std::uint64_t>(iteration));
                    {
                        ASSISI_PROFILE_SCOPE("worker-inner");
                        ASSISI_PROFILE_COUNTER("worker/iteration", static_cast<double>(iteration));
                    }
                }
            });
    }

    // Reading the shadow stacks while their owners push and pop is the whole
    // point: it is the seqlock's only real exercise. Reading the *rings* here
    // would be a different story — that needs the producers stopped — so this
    // loop deliberately touches only what SnapshotThreads can give safely.
    std::int32_t observedOpenScopes = 0;
    std::thread  reader(
        [&keepReading, &observedOpenScopes]
        {
            while (keepReading.load(std::memory_order_relaxed))
            {
                for (const Chiara::ThreadSnapshot &snapshot : Chiara::SnapshotThreads())
                {
                    for (const Chiara::OpenScope &open : snapshot.openScopes)
                    {
                        // A torn read would surface a null name or a begin from
                        // a different entry; both would trip here or in tsan.
                        if (open.name != nullptr && open.beginTicks != 0)
                        {
                            ++observedOpenScopes;
                        }
                    }
                }
            }
        });

    for (std::thread &worker : workers)
    {
        worker.join();
    }
    keepReading.store(false, std::memory_order_relaxed);
    reader.join();

    CHECK(ready.load(std::memory_order_acquire) == kThreads);
    CHECK(observedOpenScopes > 0); // The reader must actually have caught work in flight.

    // Producers are gone, so the rings are safe to read now.
    Chiara::SetRecording(false);

    std::int32_t workerThreadsSeen = 0;
    for (const Chiara::ThreadSnapshot &snapshot : Chiara::SnapshotThreads())
    {
        if (snapshot.name == nullptr || !std::string(snapshot.name).starts_with("chiara-w"))
        {
            continue;
        }
        ++workerThreadsSeen;
        CHECK(snapshot.lostEvents == 0); // Sized so nothing wrapped; a wrap here would mask corruption.

        std::int32_t outerCount = 0;
        std::int32_t innerCount = 0;
        for (std::uint64_t index = snapshot.beginIndex; index < snapshot.endIndex; ++index)
        {
            const Chiara::Event &event = snapshot.ring->At(index);
            if (event.type != Chiara::EventType::Scope)
            {
                continue;
            }
            REQUIRE(event.name != nullptr);
            if (std::strcmp(event.name, "worker-outer") == 0)
            {
                ++outerCount;
            }
            else if (std::strcmp(event.name, "worker-inner") == 0)
            {
                ++innerCount;
            }
            else
            {
                FAIL("unexpected scope name on a worker ring: " << event.name);
            }
        }
        CHECK(outerCount == kIterations);
        CHECK(innerCount == kIterations);
        CHECK(snapshot.openScopes.empty()); // Every scope closed before the thread exited.
    }

    CHECK(workerThreadsSeen == kThreads);
    Chiara::SetRecording(true);
}

#endif // ASSISI_CHIARA_ENABLED
