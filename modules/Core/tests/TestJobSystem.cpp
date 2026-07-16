/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <Assisi/Core/JobSystem.hpp>

using Assisi::Core::JobSystem;
using Assisi::Core::Pool;
using Assisi::Core::Task;

namespace
{
// Spin-drain the main queue until `predicate` holds or a generous bound is hit,
// yielding between drains so the workers make progress. Returns whether it held.
// Bounds the loop so a bug fails the test instead of hanging it.
template <class Predicate>
bool DrainMainUntil(JobSystem &jobs, Predicate predicate)
{
    for (uint32_t spins = 0; spins < 2'000'000u; ++spins)
    {
        jobs.DrainMain();
        if (predicate())
        {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}
} // namespace

TEST_CASE("JobSystem constructs with worker threads")
{
    JobSystem jobs;
    CHECK(jobs.WorkerCount() >= 1u);

    JobSystem sized(4u);
    CHECK(sized.WorkerCount() == 4u);
}

TEST_CASE("ParallelFor covers the whole range exactly once")
{
    JobSystem jobs(4u);

    constexpr uint32_t kCount = 10'000u;
    std::vector<uint32_t> out(kCount, 0u);

    jobs.ParallelFor(kCount, 64u, [&out](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i)
        {
            out[i] = i * 2u; // disjoint index-addressed write — no sharing
        }
    });

    bool allCorrect = true;
    for (uint32_t i = 0; i < kCount; ++i)
    {
        if (out[i] != i * 2u)
        {
            allCorrect = false;
            break;
        }
    }
    CHECK(allCorrect);
}

TEST_CASE("ParallelFor sums a range with no gaps or double-counting")
{
    JobSystem jobs(4u);

    constexpr uint32_t kCount = 100'000u;
    std::atomic<uint64_t> sum{0};

    jobs.ParallelFor(kCount, 128u, [&sum](uint32_t begin, uint32_t end) {
        uint64_t partial = 0;
        for (uint32_t i = begin; i < end; ++i)
        {
            partial += i;
        }
        sum.fetch_add(partial, std::memory_order_relaxed);
    });

    constexpr uint64_t kExpected = static_cast<uint64_t>(kCount) * (kCount - 1u) / 2u;
    CHECK(sum.load() == kExpected);
}

TEST_CASE("ParallelFor edge cases: empty range and single chunk")
{
    JobSystem jobs(4u);

    std::atomic<uint32_t> calls{0};
    jobs.ParallelFor(0u, 16u, [&calls](uint32_t, uint32_t) { calls.fetch_add(1u); });
    CHECK(calls.load() == 0u); // empty range never invokes the body

    // count <= grain is a single chunk: body runs once, inline, over [0,count).
    std::atomic<uint32_t> covered{0};
    jobs.ParallelFor(8u, 16u, [&covered](uint32_t begin, uint32_t end) {
        CHECK(begin == 0u);
        CHECK(end == 8u);
        covered.fetch_add(end - begin);
    });
    CHECK(covered.load() == 8u);
}

TEST_CASE("Run returns a Task whose result Wait/Get delivers")
{
    JobSystem jobs(4u);

    Task<int> task = jobs.Run(Pool::Worker, [] { return 21; });
    task.Wait();
    CHECK(task.IsComplete());
    CHECK(task.Get() == 42 / 2);
}

TEST_CASE("Then chains a value across worker stages")
{
    JobSystem jobs(4u);

    Task<int> task = jobs.Run(Pool::Worker, [] { return 21; }).Then(Pool::Worker, [](int value) { return value * 2; });
    task.Wait();
    CHECK(task.Get() == 42);
}

TEST_CASE("Then works with a void antecedent and a void result")
{
    JobSystem jobs(4u);

    std::atomic<int> effect{0};

    // void -> value: the continuation takes no argument.
    Task<int> valued =
        jobs.Run(Pool::Worker, [&effect] { effect.store(1); }).Then(Pool::Worker, [&effect] { return effect.load() + 41; });
    valued.Wait();
    CHECK(valued.Get() == 42);

    // value -> void: the continuation returns nothing.
    std::atomic<int> seen{0};
    Task<void> voided =
        jobs.Run(Pool::Worker, [] { return 7; }).Then(Pool::Worker, [&seen](int value) { seen.store(value); });
    voided.Wait();
    CHECK(seen.load() == 7);
}

TEST_CASE("Then(Main) marshals the continuation onto the DrainMain thread")
{
    JobSystem jobs(4u);

    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<bool>     ranOnMain{false};
    std::atomic<bool>     completed{false};

    jobs.Run(Pool::Worker, [] { return 7; }).Then(Pool::Main, [&](int value) {
        ranOnMain.store(std::this_thread::get_id() == mainThread && value == 7);
        completed.store(true);
    });

    REQUIRE(DrainMainUntil(jobs, [&completed] { return completed.load(); }));
    CHECK(ranOnMain.load());
}

TEST_CASE("RunOnMain queues work that only DrainMain runs")
{
    JobSystem jobs(4u);

    std::atomic<int> counter{0};
    jobs.RunOnMain([&counter] { counter.fetch_add(1); });
    jobs.RunOnMain([&counter] { counter.fetch_add(1); });

    CHECK(counter.load() == 0); // nothing ran yet — no DrainMain
    CHECK(jobs.DrainMain() == 2u);
    CHECK(counter.load() == 2);
}

TEST_CASE("DrainMain honours the maxTasks cap")
{
    JobSystem jobs(4u);

    std::atomic<int> counter{0};
    for (int i = 0; i < 3; ++i)
    {
        jobs.RunOnMain([&counter] { counter.fetch_add(1); });
    }

    CHECK(jobs.DrainMain(2u) == 2u); // only two of three
    CHECK(counter.load() == 2);
    CHECK(jobs.DrainMain(0u) == 1u); // 0 = drain the rest
    CHECK(counter.load() == 3);
}

TEST_CASE("Then on an already-complete task still runs")
{
    JobSystem jobs(4u);

    Task<int> antecedent = jobs.Run(Pool::Worker, [] { return 5; });
    antecedent.Wait(); // force completion before chaining
    REQUIRE(antecedent.IsComplete());

    Task<int> chained = antecedent.Then(Pool::Worker, [](int value) { return value + 1; });
    chained.Wait();
    CHECK(chained.Get() == 6);
}

TEST_CASE("Many concurrent tasks all complete with the right results")
{
    JobSystem jobs(4u);

    constexpr uint32_t   kTasks = 1'000u;
    std::vector<Task<uint32_t>> tasks;
    tasks.reserve(kTasks);
    for (uint32_t i = 0; i < kTasks; ++i)
    {
        tasks.push_back(jobs.Run(Pool::Worker, [i] { return i * i; }));
    }

    uint64_t sum = 0;
    for (Task<uint32_t> &task : tasks)
    {
        task.Wait();
        sum += task.Get();
    }

    uint64_t expected = 0;
    for (uint32_t i = 0; i < kTasks; ++i)
    {
        expected += static_cast<uint64_t>(i) * i;
    }
    CHECK(sum == expected);
}
