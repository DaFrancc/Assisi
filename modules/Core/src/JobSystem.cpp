/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/JobSystem.hpp>

#include <algorithm>
#include <iterator>

namespace Assisi::Core
{

JobSystem::JobSystem(uint32_t workerCount)
{
    if (workerCount == 0)
    {
        // Leave one core for the main thread; a 1-core machine still gets 1 worker
        // so background work makes progress (help-waiting keeps it deadlock-free).
        const uint32_t hardware = std::thread::hardware_concurrency();
        workerCount = hardware > 1u ? hardware - 1u : 1u;
    }

    _workers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i)
    {
        _workers.emplace_back([this] { WorkerLoop(); });
    }
}

JobSystem::~JobSystem()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping.store(true, std::memory_order_relaxed);
    }
    _wake.notify_all();
    for (std::thread &worker : _workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void JobSystem::WorkerLoop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [this] { return _stopping.load(std::memory_order_relaxed) || !_workerQueue.empty(); });
            // Drain remaining work before exiting so tasks queued right before
            // shutdown still run (e.g. a final flush).
            if (_workerQueue.empty())
            {
                return; // only reached when _stopping and the queue is empty
            }
            task = std::move(_workerQueue.front());
            _workerQueue.pop_front();
        }
        task();
    }
}

void JobSystem::EnqueueTo(Pool pool, std::function<void()> task)
{
    if (pool == Pool::Main)
    {
        std::lock_guard<std::mutex> lock(_mainMutex);
        _mainQueue.push_back(std::move(task));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _workerQueue.push_back(std::move(task));
    }
    _wake.notify_one();
}

void JobSystem::RunOnMain(std::function<void()> fn)
{
    EnqueueTo(Pool::Main, std::move(fn));
}

bool JobSystem::TryRunOneWorkerTask()
{
    std::function<void()> task;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_workerQueue.empty())
        {
            return false;
        }
        task = std::move(_workerQueue.front());
        _workerQueue.pop_front();
    }
    task();
    return true;
}

bool JobSystem::TryRunOneMainTask()
{
    std::function<void()> task;
    {
        std::lock_guard<std::mutex> lock(_mainMutex);
        if (_mainQueue.empty())
        {
            return false;
        }
        task = std::move(_mainQueue.front());
        _mainQueue.erase(_mainQueue.begin());
    }
    task();
    return true;
}

void JobSystem::ParallelFor(uint32_t count, uint32_t grain,
                            const std::function<void(uint32_t begin, uint32_t end)> &body)
{
    if (count == 0)
    {
        return;
    }
    if (grain == 0)
    {
        grain = 1;
    }

    const uint32_t chunks = (count + grain - 1u) / grain;
    if (chunks == 1u || _workers.empty())
    {
        body(0u, count); // nothing to parallelize / nowhere to run it
        return;
    }

    // `body` outlives this call (we block until every chunk finishes), so the
    // per-chunk tasks capture it by reference — no std::function copy per chunk.
    auto remaining = std::make_shared<std::atomic<uint32_t>>(chunks);
    for (uint32_t chunk = 0; chunk < chunks; ++chunk)
    {
        const uint32_t begin = chunk * grain;
        const uint32_t end = std::min(begin + grain, count);
        EnqueueTo(Pool::Worker, [&body, begin, end, remaining] {
            body(begin, end);
            remaining->fetch_sub(1u, std::memory_order_acq_rel);
        });
    }

    HelpUntil([&remaining] { return remaining->load(std::memory_order_acquire) == 0u; });
}

uint32_t JobSystem::DrainMain(uint32_t maxTasks)
{
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(_mainMutex);
        if (maxTasks == 0u || maxTasks >= _mainQueue.size())
        {
            batch.swap(_mainQueue);
        }
        else
        {
            const auto splitAt = _mainQueue.begin() + static_cast<std::ptrdiff_t>(maxTasks);
            batch.assign(std::make_move_iterator(_mainQueue.begin()), std::make_move_iterator(splitAt));
            _mainQueue.erase(_mainQueue.begin(), splitAt);
        }
    }

    for (std::function<void()> &task : batch)
    {
        task();
    }
    return static_cast<uint32_t>(batch.size());
}

} // namespace Assisi::Core
