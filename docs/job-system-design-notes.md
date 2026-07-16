# Job system & async — design notes (deferred)

Captured 2026-07-16. **Nothing here is built yet.** This records the intended
concurrency model for Assisi so the current single-threaded main loop (plus the
handful of ad-hoc threads that have already accreted) isn't mistaken for a
decision, and so the "async load on worker threads" the streaming notes assume
(`asset-streaming-design-notes.md`, layer 1) has a foundation to name.

Scope, as decided: a **general-purpose job system** for the whole engine, with
**async asset I/O as its first client** — not an asset-loader that happens to
spawn threads. The general primitive comes first; streaming, CPU render extract,
and arena compaction are consumers of it.

## Why now — concurrency is already accreting ad-hoc

The engine presents as single-threaded (main-thread `OnUpdate` → `OnRender`,
fully synchronous asset load), but three pieces of unmanaged concurrency already
exist, and a fourth is designed-in-waiting:

- **Jolt owns a thread pool.** `PhysicsWorld` constructs a
  `JPH::JobSystemThreadPool` (`hardware_concurrency - 1` threads) purely to drive
  `physicsSystem.Update` (`PhysicsWorld.cpp`). It's real, it's sized to the
  machine, and it's invisible to the rest of the engine.
- **A bespoke worker thread.** `GpuTelemetry` runs its own `std::thread` +
  `std::mutex` + `std::condition_variable` to poll GPU timing off the main thread
  (`GpuTelemetry.cpp`). A one-off.
- **The logger is already multi-thread-hardened** — `Logger` guards every call
  with a mutex *specifically because Jolt's job threads log* (see the comment in
  `Logger.hpp`). So we already pay for thread-safety without owning the threads.
- **The load path hand-rolls a main-thread marshal.** The `_pendingLevelLoad` →
  `OnUpdate` deferral (SandboxLevels/SandboxApp) exists because a load must not
  run mid-frame; it's a manual "run this on the main thread at a safe point,"
  which is exactly the primitive a job system should provide.

Two one-off threads and a hand-rolled marshal is the signal that a general
primitive is overdue. The **forcing function** is streaming layer 1: decode +
mip-gen on workers and GPU upload through a transfer queue so the main thread
never blocks (and a loading screen becomes possible at all). That work is
I/O-pipeline-shaped, not a compute DAG — which drives the model choice below.

## Goals / non-goals

**Goals**

- One engine-owned worker pool, sized against the core budget, that *all*
  background work shares — no more per-subsystem threads.
- A **main-thread marshal** as a first-class concept: any job can schedule its
  continuation to run on the main thread at a safe point (GPU submit, ECS
  mutation, and resource publish are main-thread-affine).
- Cheap **fan-out** (`parallel_for`) for embarrassingly-parallel work (texture
  decode, mip-gen, CPU cull).
- **Async chains** with thread-affinity hops (read → decode → upload → publish)
  expressible without blocking a worker.
- **Cancellation** so streamed-out / evicted requests clean up mid-flight.
- Coexistence with Jolt's scheduler without oversubscribing cores.

**Non-goals (for the first cut)**

- Fibers. (See prior art — powerful but affinity-hostile; ruled out.)
- Coroutines as the *core* scheduler. Left as a named later stage: a coroutine
  surface layered over the future/pool core, not a rewrite of it.
- A lock-free work-stealing deque on day one. Start with a correct shared queue;
  upgrade to work-stealing behind the same API once profiling asks for it.
- Job-graph GPU-cull (stage F) — that's GPU compute, a different doc.

## Prior art (why the "popular route")

Core-scheduler models, and who ships them:

- **Work-stealing pool of small tasks + pinned threads for affinity — the
  mainstream.** Unreal (`UE::Tasks` / TaskGraph over a low-level work-stealing
  scheduler, with *named threads* — Game/Render/RHI — for affinity), Unity (C#
  Job System: `IJob`/`IJobParallelFor`, `JobHandle` deps, a race-detecting safety
  layer), id Tech (parallel command-list generation), bitsquid/Stingray
  ("A Task is Not a Thread" — deliberately minimal, atomic-counter tasks, no
  fibers). Library incarnations: **enkiTS** (small, game-oriented, *pinned /
  main-thread tasks are first-class* — the closest single model to what Assisi
  should build), Intel **TBB**, **Taskflow**.
- **Fibers — the ambitious outlier.** Naughty Dog (Gyrling, GDC 2015): the whole
  frame is jobs, `WaitForCounter` yields the fiber. Famous because of one great
  talk, not because it's common. The cost is exactly our problem: a fiber can
  resume on any thread, which fights main-thread-only GPU submit.
- **Coroutines — emerging, mostly as a *surface*.** No major shipping AAA engine
  uses C++20 coroutines as its *core* scheduler as of writing; they appear at the
  async-I/O layer (Unity C# `async/await` over `AsyncOperation`, UE coroutine
  plugins, asio/folly::coro in C++). This is why we take them as a later surface,
  not the foundation.

Async asset loading, by contrast, is near-universal regardless of core: a
dedicated loading thread / small I/O pool with a **priority queue**, decode on
compute workers, **GPU upload through a transfer/copy queue** with staging
buffers, **publish on the main thread**, and **cancellation tokens**. That is
streaming layer 1 verbatim, and it's independent of the scheduler choice.

**Decision:** the mainstream camp — a work-stealing (eventually) pool with
pinned main-thread tasks, futures/callbacks surface, `parallel_for` for fan-out.
enkiTS is the explicit reference model; we build our own thin version rather than
vendor it, so the main-thread queue and NVRHI transfer integration are ours.

## The model

### Threads

One pool of **N = hardware_concurrency − 1** worker threads (leave one core for
the main thread), created once at engine start and owned by a `JobSystem`
singleton-ish service. The **main thread is not a worker** — it runs the frame
loop and drains a main-thread task queue at defined points. This keeps GPU
submit and ECS mutation single-threaded by construction.

I/O vs compute: file reads block; CPU decode/mip-gen saturate a core. Mixing
them in one pool risks a burst of blocking reads parking every worker. Two
sub-options, decided at build-out (flagged in Risks):

- **Simple:** one pool; blocking I/O jobs are tagged low-count / a soft cap
  limits concurrent I/O jobs so reads can't consume every worker.
- **Split:** a tiny dedicated I/O thread (or two) for `read`/`stat`, compute pool
  for decode. Cleaner starvation story; slightly more plumbing.

Start simple, measure, split only if reads starve compute.

### Affinity: pinned main-thread tasks

Affinity is handled by *where a continuation runs*, never by blocking a worker.
Every task targets an execution context:

- `Pool::Worker` — any worker thread.
- `Pool::Main` — the main-thread queue, drained once per frame at a safe point
  (generalizes `_pendingLevelLoad`; the load button becomes `RunOnMain([]{ … })`).

The main-thread queue is the single mechanism for "publish a finished async
result into the scene / GPU." It replaces both the `_pendingLevelLoad` special
case and any future ad-hoc "defer to OnUpdate" pattern.

### Surface API (sketch — not final)

Futures/callbacks as the foundation; a continuation names its target context.
Explicit-width types throughout (house rule).

```cpp
// Fan-out: split [0,count) across workers, join on the returned handle.
JobHandle parallel_for(uint32_t count, uint32_t grain,
                       std::function<void(uint32_t begin, uint32_t end)> body);

// Single task; returns a future whose .Then continuation runs on `where`.
template <class F> Task<std::invoke_result_t<F>> run(Pool where, F&& fn);

// Async chain with affinity hops (streaming layer 1):
run(Pool::IO,     [=]           { return ReadFile(path); })
  .Then(Pool::Worker, [=](Bytes b)  { return DecodeAndMip(b); })
  .Then(Pool::Main,   [=](Image i)  { PublishTexture(id, i); });   // main-thread publish

// Wait (main thread only, e.g. loading-screen gate):
handle.Wait();                 // or poll handle.IsComplete() while rendering UI
```

- `.Then(pool, fn)` enqueues the continuation onto `pool` when the antecedent
  completes — the affinity hop is a parameter, not a blocking wait.
- **Cancellation:** a `CancelToken` passed into a chain; each stage checks it and
  short-circuits (with a manual token, not unwinding). Coroutine unwinding would
  make this cleaner — noted as the one thing that would argue coroutines-first if
  eviction becomes frequent (see Risks).
- **Lifetime:** the future-chain footgun is dangling captures across a hop. The
  convention is value-capture of ids/handles (not raw pointers into structures
  that a `Clear()` may free); the async loader captures `AssetId`, never
  `Texture*`.

### The coroutine upgrade (deferred, hook named)

A `Task<T>` awaitable can wrap the same future + pool, so `co_await` on a job
completion becomes possible without changing the scheduler. When async chains
grow past ~4 stages, or cancellation-heavy streaming makes manual tokens painful,
add the coroutine surface *over* this core. Nothing here precludes it; do not
build it until a chain is long enough to hurt.

## Relationship to Jolt's pool (open decision — recommended default)

Two thread pools (ours + Jolt's) on one machine oversubscribe cores: both size
to `hardware_concurrency − 1`, so under load they fight. Options:

1. **One pool, adapt to Jolt (recommended).** Implement `JPH::JobSystem` over the
   engine pool so physics runs on the same threads — one core budget, no
   oversubscription. Cost: satisfy Jolt's contract (`CreateJob`, barriers, fixed
   `cMaxPhysicsJobs`/`cMaxPhysicsBarriers`). Jolt's `JobSystemWithBarrier` base
   class does much of the barrier bookkeeping; the adapter mainly forwards job
   execution to our workers.
2. **Separate pools.** Simplest to write; keep Jolt's pool an implementation
   detail of Physics. Accept oversubscription, or hand-size both against a shared
   budget.

**Recommendation: one pool with a `JPH::JobSystem` adapter**, but treat it as a
**spike to validate before committing** — physics runs a fixed-timestep,
deterministic sim, so the adapter must not perturb job ordering *within a step's
barriers*. Determinism across steps is unaffected (async I/O jobs never share a
barrier with sim jobs); the risk is purely inside Jolt's own barrier graph, which
the adapter must honor exactly. If the spike shows the adapter is fiddly or
risks determinism, fall back to separate pools sized against a shared budget.

## Interaction with existing systems

- **Streaming (client #1).** Layers 1–3 of `asset-streaming-design-notes.md` are
  the primary consumer: async decode/mip on workers, transfer-queue upload,
  main-thread publish, priority queue, cancellation on eviction. This doc is that
  doc's missing foundation.
- **Geometry-arena compaction.** The semi-space compaction in the streaming notes
  is already fence-gated against the *GPU* reader; the CPU side (choose live set,
  record the copy, queue the deferred free) is a natural worker job whose swap
  publishes on the main thread at a frame boundary. Fits without new mechanism.
- **CPU render extract / cull (stage E/F adjacent).** `DrawScene`'s extract → cull
  → sort loop over entities is a `parallel_for` candidate once it's a bottleneck
  — but the *result ordering* must stay deterministic for the sort key, so the
  parallel cull writes into per-index slots and the sort runs after the join, not
  interleaved. Not urgent; noted so the seam is known.
- **Physics.** Via the Jolt adapter above.
- **NVRHI.** Worker threads that build GPU work need their own command lists
  (NVRHI command-list creation/recording is per-list, not shared); **submission
  stays main-thread** (or a single dedicated submit thread). Uploads use the
  transfer/copy queue with a semaphore ordering copy → graphics, matching the
  arena-compaction fence discipline already designed.
- **Logger.** Already thread-safe; no change. The mutex it pays for stops being
  "for Jolt" and becomes "for the job system," which is the honest framing.

## Determinism

The fixed-timestep sim + snapshot/interpolation (`PhysicsWorld::CaptureState`)
means sim reproducibility matters. Rules that keep it:

- Async **I/O jobs never feed the sim mid-step** — results publish on the main
  thread at frame boundaries, at which point they're as deterministic as any
  main-thread mutation.
- `parallel_for` over ECS must write to **disjoint, index-addressed outputs** and
  be **order-independent**; any reduction/sort happens after the join. No
  entity's result may depend on another's completion order.
- Jolt's own determinism is Jolt's concern; the adapter's only job is to not
  reorder work inside its barriers.

## Risks / open questions

- **Jolt adapter spike** (above) — the one open decision. Validate barrier +
  determinism behavior before committing to one pool.
- **I/O starvation** — one mixed pool vs. a split I/O thread. Start mixed with an
  I/O-job soft cap; measure.
- **Work-stealing later, not first** — ship a correct shared-queue pool; only add
  per-worker deques + stealing when contention shows in a profile. Same public
  API either way.
- **Cancellation ergonomics** — manual tokens are fine for coarse (loading-screen
  / chunk-prefetch) eviction; frequent fine-grained eviction is the case that
  would argue for the coroutine surface sooner.
- **Capture lifetime** — value-capture ids/handles, never pointers a `Clear()`
  can free. Worth a lint/convention note when this is built.
- **Main-thread queue starvation / spikes** — draining unbounded work on the main
  thread at a frame boundary can spike a frame. Budget the drain (time-slice or
  max-N per frame) for streaming publishes.
- **False sharing / allocation churn** — job structs and per-worker state want
  cache-line padding; a small job allocator beats `std::function` heap traffic at
  high job counts. A later optimization, not a first-cut requirement.

## Staged rollout

1. **Core pool + main-thread queue.** N workers, shared queue, `run(pool, fn)`,
   `.Then(pool, fn)`, `parallel_for`, `RunOnMain`. Port `GpuTelemetry`'s bespoke
   thread onto it and convert `_pendingLevelLoad` to `RunOnMain` as the first two
   proof clients.
2. **Async asset load (streaming layer 1).** File read + decode + mip on the pool,
   transfer-queue upload, main-thread publish, priority queue, cancel tokens.
   Unblocks the loading screen (layer 2).
3. **Jolt adapter** (after the spike) — collapse to one pool.
4. **parallel_for the CPU render extract/cull** if/when it profiles hot.
5. **Coroutine surface** — only if chains grow or cancellation gets heavy.
6. **Work-stealing + job allocator** — behind the same API, when contention shows.

## Related

- `asset-streaming-design-notes.md` — the primary client (layers 1–3); this doc
  is its missing foundation.
- `gpu-driven-rendering-design-notes.md` / `mesh-material-architecture.md` — stage
  F GPU-cull is GPU compute, not this system; CPU extract is a `parallel_for`
  client.
- `frame-profiler-design-notes.md` — worker threads need profiler scopes too.
