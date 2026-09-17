/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file JoltRuntime.cpp
/// @brief The Jolt state that is process-wide rather than per world: the
///        library globals and the worker pool every PhysicsWorld steps on.

#include "PhysicsInternal.hpp"

#include <Assisi/Chiara/Chiara.hpp>
#include <Assisi/Core/Logger.hpp>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/RegisterTypes.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

namespace Assisi::Physics
{

namespace
{

/* The Jolt state that is genuinely process-global and worth sharing across every
   PhysicsWorld: the library globals (allocator, Factory, type registration) and
   the job-system thread pool. Multi-scene runs several PhysicsWorlds side by side
   a pool per world would spawn
   hardware_concurrency() threads *per resident level* and oversubscribe the
   machine, so the pool is shared and every world's Update() dispatches onto it.

   Refcounted rather than a leaked singleton so a process that stops using physics
   gives its worker threads back, and so construction order is correct by
   construction: the globals are registered before the pool is built (Jolt
   allocates through its own allocator, which RegisterDefaultAllocator installs),
   and the pool outlives every PhysicsWorld that could still be stepping —
   PhysicsWorld::Impl holds its handle as its first member, so it is acquired
   before any other Jolt object of that world and released after all of them.
   Atomic so worlds constructed/destroyed on different threads can't lose a count
   and double-free the factory.

   The scratch allocator is deliberately NOT here — it is per-world (see Impl).
   TempAllocatorImpl is a stack, used throughout a step by the pool workers a
   single Update() dispatches; two worlds' Update()s sharing one would interleave
   their frames, and the accesses cross pool-worker threads with no happens-before
   edge (a data race ThreadSanitizer flags). A per-world allocator is 10 MiB of
   scratch each — cheap — and makes stepping safe whether worlds run sequentially
   or in parallel. The pool stays shared; Jolt is built for many PhysicsSystems on
   one JobSystem. */
/* Under ThreadSanitizer the pool is replaced by Jolt's single-threaded job
   system. Jolt's solver coordinates its workers through its own barriers and
   atomics rather than anything tsan models as a happens-before edge, so a
   threaded step reports races inside `JobSystem.h` and `TempAllocator.h` — Jolt's
   own headers, instrumented only because they are inlined into this TU (the Jolt
   library does not link Assisi::Sanitize). Those reports cannot be fixed here and
   bury any real race in noise. Stepping on one thread removes them at the source
   rather than hiding them behind a suppression, and costs only speed: Jolt's
   results do not depend on worker count, and everything around physics still runs
   threaded. */
struct JoltRuntime
{
#if defined(ASSISI_PHYSICS_TSAN)
    JPH::JobSystemSingleThreaded jobSystem;

    JoltRuntime() { jobSystem.Init(JPH::cMaxPhysicsJobs); }
#else
    // Default-constructed and then Init'd in the body rather than built by the
    // thread-starting constructor: Jolt requires the thread-init function to be
    // set *before* Init, and setting it afterwards compiles fine while silently
    // doing nothing. Without this the physics workers would stay anonymous in
    // every capture and every debugger.
    JPH::JobSystemThreadPool jobSystem;

    JoltRuntime()
    {
        jobSystem.SetThreadInitFunction(
            [](int threadIndex)
            { Assisi::Chiara::RegisterCurrentThread(("jolt-" + std::to_string(threadIndex)).c_str()); });
        jobSystem.Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                       static_cast<int>(std::thread::hardware_concurrency()) - 1);
    }
#endif
};

/// Jolt allocation counters. Churn per frame, not residency: JPH::FreeFunction
/// takes no size, so tracking live bytes would need a header on every block,
/// which breaks aligned allocation. Churn is the perf-relevant signal anyway —
/// a physics frame that allocates is a physics frame that will pay for it.
///
/// Relaxed atomics because Jolt allocates from its own worker threads; these are
/// sampled once a frame, so ordering between them does not matter.
std::atomic<std::uint64_t> gJoltAllocCount{0};
std::atomic<std::uint64_t> gJoltAllocBytes{0};

void *CountingAllocate(std::size_t size)
{
    gJoltAllocCount.fetch_add(1, std::memory_order_relaxed);
    gJoltAllocBytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size);
}

void *CountingReallocate(void *block, std::size_t oldSize, std::size_t newSize)
{
    gJoltAllocCount.fetch_add(1, std::memory_order_relaxed);
    if (newSize > oldSize)
    {
        gJoltAllocBytes.fetch_add(newSize - oldSize, std::memory_order_relaxed);
    }
    return std::realloc(block, newSize);
}

void CountingFree(void *block)
{
    std::free(block);
}

void *CountingAlignedAllocate(std::size_t size, std::size_t alignment)
{
    gJoltAllocCount.fetch_add(1, std::memory_order_relaxed);
    gJoltAllocBytes.fetch_add(size, std::memory_order_relaxed);
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    // std::aligned_alloc requires size to be a multiple of alignment.
    return std::aligned_alloc(alignment, ((size + alignment - 1) / alignment) * alignment);
#endif
}

void CountingAlignedFree(void *block)
{
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

std::atomic<int32_t> gJoltRefCount{0};
JoltRuntime *gJoltRuntime = nullptr;

} // namespace

JoltRuntimeRef::JoltRuntimeRef()
{
    if (gJoltRefCount++ == 0)
    {
        /* Must be called before any Jolt allocation — including the runtime's
           own pool and temp allocator below. Counting wrappers rather than
           RegisterDefaultAllocator: all five hooks, because installing only
           some leaves the rest null and Jolt calls them all. */
        JPH::Allocate        = CountingAllocate;
        JPH::Reallocate      = CountingReallocate;
        JPH::Free            = CountingFree;
        JPH::AlignedAllocate = CountingAlignedAllocate;
        JPH::AlignedFree     = CountingAlignedFree;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        gJoltRuntime = new JoltRuntime();
        Assisi::Core::Log::Info("Jolt: runtime up ({} worker thread(s), shared by every physics world){}.",
                                gJoltRuntime->jobSystem.GetMaxConcurrency(),
#if defined(ASSISI_PHYSICS_TSAN)
                                " - single-threaded, this is a ThreadSanitizer build"
#else
                                ""
#endif
                                );
    }
}

JoltRuntimeRef::~JoltRuntimeRef()
{
    if (--gJoltRefCount == 0)
    {
        delete gJoltRuntime;
        gJoltRuntime = nullptr;
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

JPH::JobSystem &JoltRuntimeRef::JobSystem() const
{
    return gJoltRuntime->jobSystem;
}

JoltAllocationStats GetJoltAllocationStats()
{
    return JoltAllocationStats{gJoltAllocCount.load(std::memory_order_relaxed),
                               gJoltAllocBytes.load(std::memory_order_relaxed)};
}

} // namespace Assisi::Physics
