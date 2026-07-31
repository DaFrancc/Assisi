/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestOverhead.cpp
/// @brief What a scope actually costs.
///
/// The design notes carried an *estimate* through every stage. An estimate is
/// how a profiler ends up lying about the one number it has no excuse to guess,
/// so this measures it and prints the result — the self-measurement caveat
/// carried over from the deferred frame-profiler notes, discharged.
///
/// This is a measurement, not a threshold. It asserts only that instrumentation
/// is *cheap*, with a bound loose enough that a loaded CI machine or a debug
/// build cannot make it fail; the number in the output is the point.

#include "ChiaraTest.hpp"

#include <Assisi/Chiara/Profile.hpp>

#include <doctest/doctest.h>

// A sanitized build instruments every memory access, which inflates all of this
// by one to two orders of magnitude — the numbers would be measuring the
// sanitizer, and the bounds below would fail for a reason that has nothing to do
// with the code. Correctness under tsan is covered by the other suites; speed is
// not a question a sanitized build can answer.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#    define ASSISI_CHIARA_SANITIZED 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#        define ASSISI_CHIARA_SANITIZED 1
#    endif
#endif

#if defined(ASSISI_CHIARA_ENABLED) && !defined(ASSISI_CHIARA_SANITIZED)

#    include <chrono>
#    include <cstdint>
#    include <cstdio>

using Assisi::ChiaraTest::EnsureInitialized;

namespace
{

/// @brief Nanoseconds per iteration of @p body, averaged over @p iterations.
template <typename Fn>
[[nodiscard]] double NanosPerIteration(std::int32_t iterations, Fn &&body)
{
    const auto start = std::chrono::steady_clock::now();
    for (std::int32_t i = 0; i < iterations; ++i)
    {
        body();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())
           / static_cast<double>(iterations);
}

} // namespace

TEST_CASE("A scope costs what the notes claim it does")
{
    EnsureInitialized();

    constexpr std::int32_t kIterations = 200'000;

    // Recording off: one relaxed atomic load and a predicted branch. This is what
    // instrumentation costs in a -c build that is not currently capturing, which
    // is the state a shipped build spends nearly all its time in.
    Assisi::Chiara::SetRecording(false);
    const double disabledNanos = NanosPerIteration(kIterations,
                                                   []
                                                   {
                                                       ASSISI_PROFILE_SCOPE("overhead-disabled");
                                                   });

    // Recording on: two clock reads, the shadow-stack seqlock, and a 32-byte
    // ring store.
    Assisi::Chiara::SetRecording(true);
    const double enabledNanos = NanosPerIteration(kIterations,
                                                  []
                                                  {
                                                      ASSISI_PROFILE_SCOPE("overhead-enabled");
                                                  });

    const double counterNanos = NanosPerIteration(kIterations,
                                                  []
                                                  {
                                                      ASSISI_PROFILE_COUNTER("overhead/counter", 1.0);
                                                  });

    std::printf("\n[chiara] measured overhead, %d iterations each:\n"
                "         scope, not recording : %6.2f ns\n"
                "         scope, recording     : %6.2f ns\n"
                "         counter, recording   : %6.2f ns\n\n",
                kIterations, disabledNanos, enabledNanos, counterNanos);

    // Loose bounds on purpose. The measurement is the deliverable; these only
    // catch a regression that changes the order of magnitude — an accidental
    // lock on the hot path, or an allocation per event.
    CHECK(disabledNanos < 50.0);
    CHECK(enabledNanos < 500.0);
    CHECK(counterNanos < 500.0);

    // The relationship is the real invariant: not recording must be markedly
    // cheaper than recording, or the early-out is not doing its job.
    CHECK(disabledNanos < enabledNanos);
}

#endif // ASSISI_CHIARA_ENABLED && !ASSISI_CHIARA_SANITIZED
