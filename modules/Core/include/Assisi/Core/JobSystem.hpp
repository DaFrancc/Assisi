/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file JobSystem.hpp
/// @brief The engine's general-purpose task scheduler (design:
///        docs/job-system-design-notes.md, stage 1 — the core).
///
/// A pool of worker threads draining a shared task queue, plus a separate
/// main-thread queue for work that must run on the main thread (GPU submit, ECS
/// mutation, resource publish). Affinity is handled by *where a continuation
/// runs* — `Pool::Worker` or `Pool::Main` — never by blocking a worker. This is
/// the mainstream "task pool + pinned main-thread tasks" model (enkiTS / UE
/// TaskGraph / Unity Jobs family); fibers and coroutines are deliberately out
/// (see the design notes).
///
/// Two work shapes are supported:
///   - **Fan-out:** `ParallelFor(count, grain, body)` splits a range across
///     workers and blocks (help-waiting) until it's done.
///   - **Async chains:** `Run(pool, fn)` returns a `Task<T>`; `.Then(pool, fn)`
///     chains a continuation onto whichever pool you name, so a load → decode →
///     upload → publish pipeline hops threads without any worker ever blocking.
///
/// Waiting (`ParallelFor`'s internal wait, `Task::Wait`) is *help-waiting*: the
/// waiting thread runs queued worker tasks itself instead of sleeping, so a wait
/// issued from a worker can't deadlock the pool. A `Task::Wait` on the main
/// thread additionally runs queued *main* tasks, so waiting on a chain that ends
/// in `Pool::Main` completes instead of livelocking (any other pending main
/// tasks queued ahead of it run too — a blocking main-thread Wait is a drain
/// point by necessity). Call the blocking entry points (`Wait`, `ParallelFor`,
/// `DrainMain`) from the main thread or a worker — never from a thread that must
/// not run arbitrary tasks.
///
/// Not yet built (later stages in the design notes): a dedicated I/O pool /
/// `Pool::IO`, work-stealing per-worker deques, cancellation tokens, and the
/// coroutine surface. The public API is shaped so those drop in without changing
/// call sites.

#include <Assisi/Core/Assert.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Assisi::Core
{

/// @brief Where a task runs. Affinity is expressed by choosing the pool a task
/// (or continuation) targets, not by blocking. `Main` tasks run when the main
/// thread calls DrainMain(); `Worker` tasks run on the pool.
enum class Pool : uint8_t
{
    Worker,
    Main,
};

namespace detail
{
/// @brief Shared state behind a Task<T> — the value slot + the one-shot
/// continuation registered by Then(). Accessed from the producing thread and the
/// consuming thread, guarded by `mutex`. Held via shared_ptr so it outlives
/// whichever side finishes last.
template <class T>
struct TaskState
{
    using Stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    std::mutex            mutex;
    bool                  done = false;
    bool                  continuationClaimed = false; ///< A Then() was chained (stays true after it fires).
    Stored                value{};
    std::function<void()> continuation; ///< Fires once, on completion (already targets its pool).
};

// Result of applying F to a Task<T>'s value: F(value) for non-void T, F() for
// void T. Specialized on void so std::invoke_result_t<F, void> is never formed.
template <class F, class T>
struct ThenResult
{
    using type = std::invoke_result_t<F, T>;
};
template <class F>
struct ThenResult<F, void>
{
    using type = std::invoke_result_t<F>;
};

template <class T>
void Complete(TaskState<T> &state)
{
    std::function<void()> continuation;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.done = true;
        continuation = std::move(state.continuation);
    }
    if (continuation)
    {
        continuation(); // enqueues the continuation onto its target pool
    }
}
} // namespace detail

class JobSystem;

/// @brief A handle to the eventual result of a Run()/Then() task. Move-only-ish
/// value type (copyable — it shares the underlying state). Chain with Then();
/// block for the result on the main thread with Wait()/Get().
template <class T>
class Task
{
  public:
    Task() = default;
    Task(std::shared_ptr<detail::TaskState<T>> state, JobSystem *jobs) : _state(std::move(state)), _jobs(jobs) {}

    /// @brief Chain a continuation that runs on @p pool once this task completes,
    /// receiving this task's result (or no argument if T is void). Returns a Task
    /// for the continuation's own result, so chains compose. At most one Then()
    /// per task (asserted): the continuation slot is one-shot and the result is
    /// *moved* into the continuation, so a second chain would silently orphan the
    /// first (its Wait() then livelocks). Fan-out belongs at the fn level — chain
    /// one continuation that dispatches.
    template <class F>
    auto Then(Pool pool, F fn) -> Task<typename detail::ThenResult<F, T>::type>;

    /// @brief True once the result is available. Cheap; safe from any thread.
    bool IsComplete() const
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        return _state->done;
    }

    /// @brief Help-wait until complete (runs queued worker tasks while waiting).
    /// Main-thread / gate use (e.g. a loading screen polling to completion).
    void Wait();

    /// @brief The result (moved out). @pre IsComplete(). Only for non-void T.
    template <class U = T, class = std::enable_if_t<!std::is_void_v<U>>>
    U Get()
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        return std::move(_state->value);
    }

    bool IsValid() const { return static_cast<bool>(_state); }

  private:
    std::shared_ptr<detail::TaskState<T>> _state;
    JobSystem                            *_jobs = nullptr;
};

class JobSystem
{
  public:
    /// @brief Start the pool. @p workerCount 0 means auto: hardware_concurrency
    /// minus one (leave a core for the main thread), floored at 1.
    explicit JobSystem(uint32_t workerCount = 0);

    /// @brief Signals workers to stop, drains queued worker tasks, and joins.
    /// Main-queue tasks still pending at shutdown are dropped (no main thread left
    /// to run them).
    ~JobSystem();

    JobSystem(const JobSystem &) = delete;
    JobSystem &operator=(const JobSystem &) = delete;
    JobSystem(JobSystem &&) = delete;
    JobSystem &operator=(JobSystem &&) = delete;

    /// @brief Run @p fn on @p pool; returns a Task for its result. Enqueues
    /// immediately.
    template <class F>
    auto Run(Pool pool, F fn) -> Task<std::invoke_result_t<F>>
    {
        using R = std::invoke_result_t<F>;
        auto state = std::make_shared<detail::TaskState<R>>();
        EnqueueTo(pool, [state, fn = std::move(fn)]() mutable {
            if constexpr (std::is_void_v<R>)
            {
                fn();
            }
            else
            {
                state->value = fn();
            }
            detail::Complete(*state);
        });
        return Task<R>{state, this};
    }

    /// @brief Fire-and-forget a task on the main-thread queue. Shorthand for
    /// scheduling work that must run on the main thread (the generalization of the
    /// hand-rolled "defer to OnUpdate" pattern).
    void RunOnMain(std::function<void()> fn);

    /// @brief Run @p body over sub-ranges of [0, count) across the workers, in
    /// chunks of @p grain, blocking (help-waiting) until every chunk is done.
    /// @p body is invoked as body(begin, end) with disjoint half-open ranges. For
    /// determinism, body must be order-independent and write to disjoint outputs;
    /// any reduction runs after this returns. A grain of 0 is treated as 1; a
    /// single chunk (or an empty pool) runs inline on the caller.
    void ParallelFor(uint32_t count, uint32_t grain, const std::function<void(uint32_t begin, uint32_t end)> &body);

    /// @brief Run pending main-thread tasks. Call once per frame from the main
    /// thread at a safe point. @p maxTasks caps how many run this call (0 = all);
    /// a cap lets a burst of streaming publishes spread across frames instead of
    /// spiking one. Tasks enqueued *by* the tasks run here are deferred to the next
    /// DrainMain. @return the number of tasks run.
    uint32_t DrainMain(uint32_t maxTasks = 0);

    /// @brief Number of worker threads (excludes the main thread).
    uint32_t WorkerCount() const { return static_cast<uint32_t>(_workers.size()); }

    // --- Used by Task<T> / Run(); public so the header templates can reach them.

    /// @brief Enqueue a type-erased task onto @p pool.
    void EnqueueTo(Pool pool, std::function<void()> task);

    /// @brief Spin running queued worker tasks until @p done() is true. The
    /// waiting thread makes progress itself rather than sleeping, so a wait from a
    /// worker can't deadlock the pool.
    ///
    /// When @p helpMain is true and the caller is the main thread (the thread
    /// that constructed the JobSystem), queued main-thread tasks are run too.
    /// Task::Wait needs this: a chain ending in Pool::Main can only complete via
    /// the main queue, so a main-thread Wait() that never drained it would
    /// livelock. ParallelFor keeps it false — its chunks are all worker tasks,
    /// and main tasks should otherwise only run at the frame's DrainMain safe
    /// point, not mid-dispatch.
    template <class Predicate>
    void HelpUntil(Predicate done, bool helpMain = false)
    {
        const bool onMainThread = helpMain && std::this_thread::get_id() == _mainThreadId;
        while (!done())
        {
            if (TryRunOneWorkerTask())
            {
                continue;
            }
            if (onMainThread && TryRunOneMainTask())
            {
                continue;
            }
            std::this_thread::yield();
        }
    }

  public:
    /// @brief Tasks waiting on the worker queue. A sampled counter, not a
    /// synchronization primitive: it is read once a frame to plot how deep the
    /// backlog is getting, which is what distinguishes "the pool is saturated"
    /// from "the pool is idle and the main thread is the problem".
    [[nodiscard]] uint32_t WorkerQueueDepth() const { return _workerQueueDepth.load(std::memory_order_relaxed); }

    /// @brief Tasks waiting to run on the main thread — see WorkerQueueDepth.
    [[nodiscard]] uint32_t MainQueueDepth() const { return _mainQueueDepth.load(std::memory_order_relaxed); }

  private:
    /// @p workerIndex names the thread for the profiler and the OS debugger.
    void WorkerLoop(uint32_t workerIndex);

    /// @brief Pop and run one worker task if any is queued. @return whether one ran.
    bool TryRunOneWorkerTask();

    /// @brief Pop and run the oldest main-queue task if any is queued. Main
    /// thread only (help-waiting). @return whether one ran.
    bool TryRunOneMainTask();

    std::vector<std::thread>          _workers;
    std::deque<std::function<void()>> _workerQueue;
    std::mutex                        _mutex; ///< Guards _workerQueue.
    std::condition_variable           _wake;  ///< Workers sleep on this when idle.
    std::atomic<bool>                 _stopping{false};

    std::vector<std::function<void()>> _mainQueue;
    std::mutex                         _mainMutex; ///< Guards _mainQueue.

    /// Queue sizes mirrored as atomics so a profiler can sample them without
    /// taking the queue locks — reading a depth should never contend with the
    /// pool it is measuring.
    std::atomic<uint32_t> _workerQueueDepth{0};
    std::atomic<uint32_t> _mainQueueDepth{0};

    /// The thread that constructed the JobSystem — treated as the main thread
    /// for HelpUntil's main-queue help (matches Application, which owns the
    /// JobSystem on the main thread).
    std::thread::id _mainThreadId = std::this_thread::get_id();
};

// --- Task<T> out-of-line template members (need the full JobSystem) -----------

template <class T>
void Task<T>::Wait()
{
    _jobs->HelpUntil(
        [this] {
            std::lock_guard<std::mutex> lock(_state->mutex);
            return _state->done;
        },
        /*helpMain=*/true);
}

template <class T>
template <class F>
auto Task<T>::Then(Pool pool, F fn) -> Task<typename detail::ThenResult<F, T>::type>
{
    using R2 = typename detail::ThenResult<F, T>::type;

    auto       next = std::make_shared<detail::TaskState<R2>>();
    auto       antecedent = _state;
    JobSystem *jobs = _jobs;

    // Runs on `pool` once the antecedent is complete: apply fn to the antecedent's
    // value, store the result in `next`, and fire next's continuation.
    auto work = [antecedent, next, fn = std::move(fn)]() mutable {
        if constexpr (std::is_void_v<T>)
        {
            if constexpr (std::is_void_v<R2>)
            {
                fn();
            }
            else
            {
                next->value = fn();
            }
        }
        else
        {
            if constexpr (std::is_void_v<R2>)
            {
                fn(std::move(antecedent->value));
            }
            else
            {
                next->value = fn(std::move(antecedent->value));
            }
        }
        detail::Complete(*next);
    };

    // The continuation registered on the antecedent enqueues `work` onto `pool`;
    // it never runs the work inline, so completion on a worker doesn't run the
    // next stage on the wrong thread.
    auto enqueueWork = [jobs, pool, work = std::move(work)]() mutable { jobs->EnqueueTo(pool, std::move(work)); };

    bool alreadyDone = false;
    {
        std::lock_guard<std::mutex> lock(antecedent->mutex);
        ASSISI_ASSERT(!antecedent->continuationClaimed,
                      "Then() called twice on the same task — the continuation slot is one-shot, so the "
                      "first chain would be silently orphaned and its Wait() would livelock");
        antecedent->continuationClaimed = true;
        if (antecedent->done)
        {
            alreadyDone = true;
        }
        else
        {
            antecedent->continuation = std::move(enqueueWork);
        }
    }
    if (alreadyDone)
    {
        enqueueWork();
    }

    return Task<R2>{next, jobs};
}

} // namespace Assisi::Core
