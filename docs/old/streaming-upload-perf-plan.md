# Streaming Frame-Stability Plan (main-thread residency)

Branch: `multi-scene`. **Goal: frame-time stability while assets stream.** All
streaming work happens on other threads; the main thread must never hitch, no
matter how large the asset or how long the total load takes. Load *latency* is
explicitly not a concern — a seamless preload may take as long as it likes, as
long as the player can't tell it's happening. Reducing total work is a side
benefit (it makes loads quicker), never the justification for an item.

> This reframes the earlier revision of this doc, which was organized around
> making uploads *cheaper*. Cheaper was the wrong primary objective; several of
> its priorities change under the stability lens (thread contention rises,
> BC7/KTX2 precooking drops out of the main sequence).

**Success criterion (testable):** *no unbounded main-thread work in the
streaming path.* Worst-case per-frame main-thread streaming cost must be O(1)
in asset size — bounded by budget constants (`PumpPublishes` time/byte budgets)
and per-item constants, never by the vertex/byte count of any single asset.
This matters doubly because the pump's "always publish at least one" escape
hatch (AssetCache.cpp:833-839, required so an over-budget asset can't wedge
forever) makes a single item's publish cost the worst-case frame cost — so
per-item main-thread cost *is* the stability contract. A softer second
criterion covers contention: streaming threads must not preempt the main
thread, observable only as p99/worst frame time (see "How to measure").

The foreground tier (10 ms / 128 MB when no seamless preload is active,
EditorApp.cpp:728) is a deliberate exception: behind a blocking load there is
nothing to protect. The 2 ms / 16 MB seamless tier (EditorApp.cpp:726) is the
contract this doc is about.

Everything below is verified against the code on this branch and the **vendored
nvrhi source** the engine builds
(`out/build/gcc-debug/_deps/nvrhi-src/src/vulkan/`), except where marked
*unmeasured* or *assumed*.

## What is on the main thread today (the inventory)

Streaming pipeline: resolve (main, O(1) kick) → decode/import (worker) →
continuation enqueues a `PendingPublish` (main, O(1), in `DrainMain` at
Application.cpp:342) → `PumpPublishes` drains under budget at the frame safe
point (EditorApp.cpp:725-728, after DrainMain, before RenderFrame) →
`FlushUploads` submits one batch.

Per item, on the main thread, ordered by severity:

| # | Work | Where | Cost class | Status |
|---|------|-------|-----------|--------|
| 1 | Mesh arena staging memcpy: `writeBuffer` of vertex+index bytes memcpys into an nvrhi upload chunk on the recording thread | `PublishMesh` (AssetCache.cpp:714-720) → `MeshBuffer::Upload` (MeshBuffer.hpp:54) → `GeometryArena::Allocate` (GeometryArena.hpp:100-103) → nvrhi `CommandList::writeBuffer` staging path (vulkan-buffer.cpp:488-499) | **O(mesh bytes) — UNBOUNDED.** ~1.9 ms ship for the 36 MB car_lod mesh; scales linearly | **R1 below** |
| 2 | Arena grow: `createBuffer` (vkCreateBuffer + vkAllocateMemory, device-local, tens of MB) + record prefix `copyBuffer` | `GeometryArena::Grow` (GeometryArena.hpp:144-181, createBuffer at :159) | Unpredictable driver-side CPU; the prefix copy itself is GPU-side (recording is O(1)). *Unmeasured* — the pump diagnostic folds it into `mesh ms` | **R2 below** |
| 3 | Streaming re-resolve sweep: `ResolveSceneAssets` over every `MeshRenderer` in every resident world, every frame, while ANY load is pending cache-wide | EditorApp.cpp:735-745 (`ForEach` → `UpgradeStreamingAssets`, LevelRuntime.cpp:62-73) plus a second sweep of the incoming world in `WorldManager::PumpPendingLoad` (World.cpp:267); sweep body AssetResolve.cpp:37-41 | O(entities × material slots) hash lookups per frame per world — O(scene), not O(1). Continuous drag, not a spike (316-entity test scene; *unmeasured in ship*). First touch of each material also does `ReadText` + parse of the `.amat` **on main** (AssetCache.cpp:519-527) — synchronous file IO, cold-cache read can be ms | **R4 below** |
| 4 | `writeDescriptorTable` (bindless registration) — must stay on main per nvrhi | `PublishMaterial` → `RegisterBindlessTexture` (AssetCache.cpp:722-772) | Bounded per material (≤5 channels); **measured 0.00 ms**. Would only matter if very many distinct textures published in one pump — the byte budget already bounds that | Acceptable |
| 5 | `FlushUploads`: one `executeCommandLists` for the whole batch | AssetCache.cpp:784-807 | **Measured ~0.1 ms**, one vkQueueSubmit regardless of batch size | Acceptable |
| 6 | Decode continuations (`OnMeshLoaded` / `OnMaterialLoaded`) | AssetCache.cpp:595-625, 694-712, run in `DrainMain` | O(1) — enqueue a `PendingPublish`, move-only | Done (P0b) |
| 7 | Material row write (96 B, inline `vkCmdUpdateBuffer`), texture `Adopt`, `Material::Create` | `PublishMaterial`, `WriteMaterialToTable` (AssetCache.cpp:449-481) | Bounded, µs | Done (P0/P1) |
| 8 | `Clear()` → `waitForIdle` full stall | AssetCache.cpp:888-897 | Deliberate — level unload, not the streaming path | By design |

Not in the streaming path but worth knowing: `ResolveTexture` on a real file
path decodes + mip-gens **synchronously on main** (AssetCache.cpp:255-260 →
`LoadFromAssets`). Streaming materials never hit it (workers decode channels;
publish only resolves `prim://` fallbacks), but any future direct caller with a
disk path would reintroduce a main-thread decode. Same for `ResolvePrimitive`'s
self-contained upload (fine — primitives are tiny and built in-process).

## What has already landed, and what it bought

All committed on `multi-scene`; working tree clean.

- **P0 + P0b** (`708f9a4`) — shared upload command list + budgeted publish
  queue. `AssetCache` owns one persistent `_uploadList` with a 16 MB staging
  chunk (`kUploadChunkSize`, AssetCache.cpp:84, created at :114-118); every
  streaming publish records into it; `PumpPublishes(timeBudgetMs, byteBudget)`
  (AssetCache.cpp:809-886) drains once per frame at the safe point and
  `FlushUploads` submits everything in **one** `executeCommandLists`. Decode
  continuations became O(1). Retired `SetMainThreadTaskBudget` as the streaming
  throttle (mechanism kept in `Application`). `Clear()` recreates the list to
  return peak staging (upload managers never shrink).
  *Stability effect:* per-frame publish work became budget-bounded; per-item
  submit/alloc overhead (once the dominant *count* of main-thread ops) went to
  one submit per frame.
- **P1** (`c7a6a8b`) — material channel textures are created **and recorded on
  the decode worker** (`DecodeAndRecordMaterialChannels`, AssetCache.cpp:627-676):
  decode, `Texture::CreateImage`, `RecordMips` into one worker command list,
  returned closed in the bundle. `PublishMaterial` on main only hands the list
  to the batch, adopts textures (dedup by (path, space)), registers bindless,
  writes the 96 B row. `Texture::UploadDecoded` split into free-threaded
  `CreateImage` + `RecordMips` + `Adopt` (Texture.cpp:203-228, 218-243 region).
  *Stability effect:* the `writeTexture` staging memcpy — previously the
  dominant unbounded main-thread cost (a 2048² RGBA8 + mips ≈ 22 MB per
  channel) — left the main thread entirely. Measured: **materials cost 0.00 ms
  on main.**
- **Bounds fix** (`f9bdd46`) — whole-mesh bounds fitting moved to the import
  worker. `MeshData` gained `LocalBounds`/`LocalAabb`/`BoundsComputed`;
  idempotent `Geometry::EnsureMeshBounds` (MeshData.hpp:176-193) runs at the end
  of `ImportMesh` (MeshImporter.cpp:664); `MeshBuffer::Upload` reads instead of
  recomputing (MeshBuffer.hpp:67-74); `EnsureSubMeshTables` shares the same fit.
- **Pump diagnostic** (`db406e8`) — per-phase timing in `PumpPublishes`
  (AssetCache.cpp:876-885), logged when a pump ≥ 2 ms: `mesh Nx / mat Nx /
  flush` split plus queue depth.

### Measured evidence (2026-07-24, RTX 3070, debug build, seamless preload)

Four spike pumps, all the same shape:

```
AssetCache pump 66.66 ms: mesh 1x 66.54 ms, mat 0x 0.00 ms, flush 0.12 ms; 0 queued
AssetCache pump 76.18 ms: mesh 1x 76.06 ms, mat 0x 0.00 ms, flush 0.11 ms; 0 queued
```

One mesh publish was the entire spike: `assets/models/car_lod/car_lod.gltf`,
**619,635 vertices / 1,952,460 indices**. At 48 B/vertex (`Geometry::Vertex`:
vec3+vec3+vec2+vec4, MeshData.hpp:30-37) that is 28.4 MiB of vertices + 7.4 MiB
of indices ≈ **36 MB**. Root cause was `MeshBuffer::Upload` re-fitting
whole-mesh bounds on main — three passes over the vertex array. Benchmarked at
that exact vertex count:

| | ComputeBoundingSphere | ComputeAabb | staging memcpy | total |
|---|---|---|---|---|
| Debug `-O0` | 39.3 ms | 21.8 ms | 2.9 ms | ~64 ms |
| Ship `-O2` | 2.4 ms | 0.7 ms | 1.9 ms | ~5 ms |

**Caveat that must not get lost: ~95% of the 66-76 ms headline was a `-O0`
artifact** (glm is ~20× slower unoptimized). The bounds fix removed that cost
from main regardless — but the honest ship-build picture before the fix was a
~5 ms hitch, of which ~1.9 ms (the memcpy) remains today. All future numbers
come from the **ship build** (`make gs`), and the metric is worst-frame / p99
frame time during a preload, not average pump cost — jitter is what matters.

## Verified nvrhi facts (load-bearing; line numbers from the vendored source)

- **Multithreaded recording is supported/intended**: ProgrammingGuide (~line 36)
  blesses recording multiple command lists concurrently and executing in any
  order; `CommandList::open` → `Queue::getOrCreateCommandBuffer` is
  mutex-guarded, "free-threaded" (vulkan-queue.cpp:83-85).
- `createTexture`, `createBuffer`, `createCommandList` are free-threaded —
  device-level Vulkan calls (internally synchronized), no nvrhi shared-state
  mutation, no external-sync requirement.
- **Submission stays on the main thread.** `Queue::submit`
  (vulkan-queue.cpp:122-129) documents that the wait/signal semaphore lists are
  persistent queue state, so `queueWaitForSemaphore`/`queueSignalSemaphore`
  "will not work well with multi-threaded command list submission to the same
  queue" — and the swapchain path uses exactly those
  (VulkanContext.cpp:996-997). `writeDescriptorTable` also stays on main.
- `executeCommandList` is **not** GPU-blocking: `Queue::submit` is a
  mutex-guarded `vkQueueSubmit` + timeline-semaphore signal, returns
  immediately. `executeCommandLists(lists, count)` (vulkan-device.cpp:650)
  issues **one** `vkQueueSubmit` for the array.
- `writeBuffer` ≤64 KB (offset 4-aligned) is inline `vkCmdUpdateBuffer`; larger
  writes suballocate an upload chunk and **memcpy on the recording thread**
  (vulkan-buffer.cpp:473-499). `writeTexture` does the same row-by-row
  (vulkan-texture.cpp:474+). This is why "record on the worker" moves the real
  cost, not just bookkeeping.
- **Every nvrhi buffer is created with `eTransferSrc | eTransferDst`
  unconditionally** (vulkan-buffer.cpp:48-49). Newly verified — this closes the
  open question on R1: the arena buffers are valid `copyBuffer` destinations
  and a `cpuAccess=Write` staging buffer is a valid source, with no desc
  changes.
- `copyBuffer` (vulkan-buffer.cpp:225-261) auto-barriers src→CopySource,
  dst→CopyDest, and tracks `cpuAccess != None` buffers as
  `referencedStagingBuffers` — the submit keeps a staging buffer alive until
  the GPU retires it, so the publisher can drop its handle immediately.
- `mapBuffer` waits only if the buffer was used in a submitted command list
  (`lastUseCommandListID != 0`, vulkan-buffer.cpp:585-589); a fresh buffer maps
  with **no GPU wait**. Only `isVolatile` buffers are pre-mapped, so mapping a
  plain `cpuAccess=Write` buffer cannot double-map.
- `writeBuffer` on a `cpuAccess=Write` buffer is **invalid** in nvrhi ("Using
  writeBuffer on mappable buffers is invalid", vulkan-buffer.cpp:488-503) — the
  staging design must use map/memcpy/unmap + `copyBuffer`, which it does.
- *Coherency wrinkle (newly found, assessed no-new-risk):* `cpuAccess=Write`
  requests only `eHostVisible` — not `eHostCoherent`
  (vulkan-allocator.cpp:28-45) — and `unmapBuffer` never flushes (explicit TODO,
  vulkan-buffer.cpp:626-634). Spec-strict, a non-coherent memory type would need
  `vkFlushMappedMemoryRanges`. But nvrhi's own `UploadManager` chunks — the path
  under *every* existing `writeBuffer`/`writeTexture` in the engine — are the
  identical mechanism (`cpuAccess=Write`, persistently mapped, memcpy, no flush;
  vulkan-upload.cpp:49-57). Any memory type where the engine currently works,
  the staging design works. Desktop drivers' first HOST_VISIBLE type is
  coherent in practice.
- **nvrhi emits no Vulkan queue-family ownership transfers** — every barrier
  uses `VK_QUEUE_FAMILY_IGNORED`, all resources are
  `VK_SHARING_MODE_EXCLUSIVE`. Basis for the transfer-queue rejection below.
- Upload/scratch managers never shrink their working set; releasing and
  recreating the command list is the only way to reclaim staging memory
  (done in `Clear()`, AssetCache.cpp:924-931).

## R1 — DONE and verified on hardware (2026-07-24, ship build, RTX 3070)

Implemented as designed below (`7929ad1`), plus a follow-up that turned out to
matter more than the handoff itself (`9fea454`). Streaming a 620k-vertex /
~36 MB mesh into a live world now produces **no frame over 8 ms**.

**The surprise: allocation churn, paid in the wrong frame.** After R1 landed,
spikes remained. Frame-phase instrumentation showed `unaccounted` ≈ 0 on every
slow frame — the main thread was never descheduled, so **contention was not the
cause and R3 would not have fixed it** (a useful negative result: R3 stays on
the list, but it was not this). The cost was in `RenderFrame`; splitting that
put 9.9–30.8 ms in `EndFrame` with the GPU near idle. Everything in `EndFrame`
except `runGarbageCollection()` is already folded into `_lastGpuWaitMs`, which
read ~0.1 ms — so by elimination it was GC, which releases every retired
submit's resources on the main thread at the end of *every* frame.

The churn was largely self-inflicted by P1: each material load created a command
list with a 16 MB minimum upload chunk (`kUploadChunkSize`, correct for the
shared list, badly wrong per-load), used it once, and dropped it. R1 added a
staging buffer per mesh on top. **Neither cost is paid where it is caused** —
it surfaces in a later, unrelated frame, which is why the pump diagnostic
happily read `mesh 0.00 ms` while frames still hitched. Fix: pool both
(AssetCache.cpp). Command lists recycle immediately (nvrhi fences their chunk
reuse); staging buffers are parked behind an `EventQuery` and reclaimed by
`pollEventQuery` — polled, never waited on, so a GC spike is never traded for a
GPU stall.

**Measured, same scenario, ship build:**

| | before pooling | after |
|---|---|---|
| Slow frames (>8 ms) during a preload | 9–11 | **0** |
| Max `runGarbageCollection` | 30.8 ms | **0.00 ms** while streaming |
| Pumps ≥2 ms | 3 (2.75–6.23 ms) | **0** |

The only remaining slow frames are outside the streaming window: startup
(`imgui 5.20`, `gc 4.92` — cold pools filling, one-time) and the Play
transition (`imgui 10.89`, `gc 0.00` — entirely editor UI, not streaming, and
absent from a Game build). Cold-pool warm-up is confirmed rather than inferred:
`gc 4.92` on the first load, `gc 0.00` on every frame after.

> **Instrumentation was temporary, and the debt is paid** (2026-07-31). The
> slow-frame phase breakdown (Application.cpp), the render sub-phase timings,
> `VulkanContext::GetLastGcMs` and `PumpPublishes`'s phase log were ad-hoc by
> design, and every one of them is now gone — replaced by **Chiara**
> (`docs/chiara-design-notes.md`), the capture system this section called for.
>
> What replaced what: the phase breakdown is scopes under a `Frame` slice; the
> render sub-phases are `begin-frame`/`scene`/`post-process`/`imgui`/`end-frame`;
> `GetLastGcMs` is the `gpu-gc` scope plus a `render/gc-ms` counter; the pump log
> is `pump-publishes` and its children with the asset path as an arg. The
> slow-frame `Log::Info` survives as a single line naming the frame index and
> pointing at a capture.
>
> The two findings that made this hunt work are the two the replacement had to
> keep, and both are stronger now. The *unaccounted* figure — which is how
> contention was ruled out — is a scrubbable track rather than a number in a log
> line, and it is finally correct: the render bracket contains the GPU wait while
> `cpuMs` excludes it, so the original arithmetic under-reported by the whole
> wait. And deferred cost, the thing that actually found the culprit, is now
> drawn: each parked staging batch carries a flow id from the frame that parks it
> to the later frame that reclaims it.

## Remaining work, in stability-impact order

### R1 (design, as built) — Mesh staging-buffer handoff: the memcpy moves to the worker  [removes the last unbounded main-thread publish cost]

The staging memcpy in row 1 of the inventory is O(mesh bytes) on main. Design
(verified against nvrhi as noted above):

- **Worker** (end of the mesh import job): `device->createBuffer` with
  `cpuAccess = CpuAccessMode::Write` sized vertices+indices (one buffer, two
  regions, or two buffers — implementer's choice), `mapBuffer` (no GPU wait on
  a fresh buffer), memcpy vertices and indices in, `unmapBuffer`. Return the
  staging handle(s) + counts in the mesh bundle instead of relying on main to
  copy out of `MeshData`.
- **Main** (`PublishMesh`): ensure capacity / reserve the arena range (bump
  cursor arithmetic, O(1)), record two `copyBuffer(arenaBuf, arenaOffset,
  staging, srcOffset, bytes)` into the shared upload list, drop the staging
  handle (the submit keeps it alive until retire). Recording a copy is
  microseconds and **O(1) in mesh size**.

Why this shape and not "reserve the offset on main at kick, worker records
`writeBuffer` at that offset": the worker never needs to know its arena offset,
so a concurrent arena grow cannot invalidate its work. The naive alternative
has a real hazard — a grow between reserve and submit swaps the buffer handle
and GPU-copies only the *used* prefix, silently losing a not-yet-submitted
worker write aimed at the old handle past the copied prefix. The staging design
is immune: the worker's product is a self-contained buffer; main binds it to
the arena-of-the-moment at publish time. (It also sidesteps nvrhi's "writeBuffer
needs the size at reserve time" and the mappable-buffer restriction.)

Verification status: buffer usage flags ✅ (unconditional TRANSFER_SRC/DST),
`createBuffer` free-threaded ✅, `mapBuffer` no-wait-on-fresh ✅, staging
lifetime across the submit ✅, coherency assessed identical-to-existing-path ✅.
Cost note: this adds a vkCreateBuffer + vkAllocateMemory per mesh **on the
worker** — acceptable (off main); pool staging buffers later only if worker-side
cost ever matters.

Side benefit (not the justification): one less CPU→CPU copy than the
upload-manager path in some cases; negligible.

### R2 — Arena grow off the publish path  [unpredictable driver cost on main]

`GeometryArena::Grow` runs `createBuffer` (device-local vkAllocateMemory,
potentially tens of MB) on main mid-publish. Initial capacities are 4 MB vertex
/ 2 MB index (GeometryArena.hpp:51-52) — car_lod alone is 28.4 MiB of vertices,
so a real level forces grows immediately and repeatedly (geometric doubling,
GeometryArena.hpp:150). The cost is *unmeasured* (the pump diagnostic folds it
into `mesh ms`); measure before building anything beyond the first bullet.

In cheap-first order:

1. **Raise initial capacities** (e.g. 64 MB vertex / 32 MB index ≈ one mid-size
   level) — one line each, removes most grows outright. VRAM cost is the trade;
   fine at current scale.
2. **Pre-reserve at preload start** once a manifest knows the incoming
   geometry total (fits the planned `.aast` relationship files) — no mid-stream
   grow at all.
3. **Worker-created grow buffer** if grows must remain: `createBuffer` is
   free-threaded, so when the pump sees queued mesh bytes exceeding remaining
   capacity it can kick a worker job to create the grown buffer; main then only
   records the prefix `copyBuffer` (O(1)) and swaps the handle. Only worth it
   if (1)/(2) prove insufficient in measurements.

### R3 — Thread contention: low-priority IO pool + a thread census  [likely the dominant residual jitter]

Frame jitter can occur with **zero** main-thread work if the OS preempts the
main thread in favor of streaming workers. Current thread population on a
16-hardware-thread machine: Jolt's shared pool spawns `hardware_concurrency()-1`
= 15 threads (PhysicsWorld.cpp:139-141; the log line "Jolt: runtime up (16
worker threads…)" reports `GetMaxConcurrency()`, which counts the calling
thread too — it is 15 spawned threads, not 16), `Core::JobSystem` spawns
another `hardware_concurrency()-1` = 15 (JobSystem.cpp:10-18), plus main:
**up to 31 runnable threads on 16 hardware threads.** P1 made this *worse* for
streaming, deliberately: decode workers now also run `createTexture` +
command-list recording (the big memcpys), and R1 adds the mesh staging memcpy
to workers too. The work went off-main; the *scheduling pressure* did not.

- Build **`Pool::IO`** — designed but not built ("Not yet built" note,
  JobSystem.hpp:33-34): 1-2 threads at below-normal OS priority, and route
  decode/import (and R1's staging fill) to it. On Linux, SCHED_OTHER thread
  priority is the per-thread nice value via `setpriority(PRIO_PROCESS,
  gettid(), n)` (`pthread_setschedparam` only carries a priority for the
  realtime policies); on Windows,
  `SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL)`. The OS then preempts
  streaming instead of the frame threads — the structural fix, vs. tuning
  budgets around contention. Precedent: UE's AsyncLoadingThread, Unity's
  background loading thread.
- **Cap total threads across Jolt + jobs.** Two full-size pools is 2×
  oversubscription before streaming even starts; consider sizing the two pools
  jointly (e.g. jobs = cores − Jolt's typical live concurrency) — needs its own
  evaluation, noted here so it isn't lost.
- Keep the in-flight load cap (`_maxConcurrentLoads = 3`,
  AssetCache.cpp:562); byte-weighting it (≤N decoded MB in flight, not 3
  items) remains a good refinement once `Pool::IO` exists — it bounds worker
  memory pressure, which is itself a stability input (allocator churn, cache
  eviction).
- *Assumed, not measured:* that residual jitter after R1 is contention-shaped.
  The pump diagnostic distinguishes it: all phases low but pump total high ⇒
  the main thread was preempted mid-pump (see AssetCache.cpp:876-881 comment).
  Whole-frame p99 in ship is the real arbiter.

### R4 — Event-driven streaming upgrade instead of the per-frame sweep  [O(scene) per frame → O(what landed)]

While any load is pending cache-wide, **every** resident world re-resolves
**every** `MeshRenderer` **every** frame (inventory row 3). Per entity it's
hash lookups + a `materials` vector rebuild — bounded per entity, but O(scene)
per frame, growing with entity count forever; and the *first* touch of each
material does synchronous `.amat` file IO on main (AssetCache.cpp:519). At 316
entities this is probably tens of µs in ship (*unmeasured — measure before
building*); it is the one item here that scales with content size rather than
asset size.

Fix shape (design, not yet committed to):

- The cache records which paths became resident this pump (`PublishMesh` /
  `PublishMaterial` already know); exposes e.g. `TakeNewlyResident()`.
- Each world keeps the set of entities still holding a placeholder (built on
  the initial resolve — the sweep already visits everything once). Per frame,
  upgrade only entities referencing newly-resident paths, then shrink the set.
  Empty set ⇒ zero per-frame work — replacing the cache-wide
  `HasPendingLoads()` gate (EditorApp.cpp:735-745) that today makes world A
  sweep because world B is loading.
- The `.amat` parse-on-main belongs in the worker load body eventually (parse
  needs the asset database for channel-id→path resolution, which is main-thread
  state — resolve ids on main at kick as today, move only the file read+parse);
  low priority while `.amat` files are small, worth remembering when they grow.

### R5 — Publish-cost guardrails (cheap, ongoing)

- Keep the pump diagnostic permanently (it is the stability regression test's
  sensor), but treat any `mesh Nx` time that scales with vertex count as a bug
  after R1 lands. **Discharged, without weakening it:** the sensor is now the
  always-on `pump-publishes` / `publish-mesh` / `publish-material` /
  `flush-uploads` scopes and the `stream/*` counters, readable from any capture.
  That is strictly more sensitive than the log line it replaces, which only
  spoke when a pump crossed a 2 ms threshold guessed before anyone knew what
  normal looked like — a slow drift under that bar was invisible to it and is
  not invisible to a counter track.
- When a genuinely enormous single asset appears (a 100 MB mesh), the escape
  hatch still publishes it in one frame — after R1 that publish is O(1) on
  main, so this stops being a stability event. The GPU copy still lands in one
  submit; if GPU frame time ever visibly dips at that submit, split the
  `copyBuffer` across pumps (the staging buffer makes partial copies trivial —
  another reason R1's shape is right).

## Demoted / rejected

- **Import-time BC7/KTX2 + pregenerated mips** (was P3) — **demoted out of this
  plan.** It reduces total work (deletes runtime mip gen, ~4× fewer bytes
  decoded/staged/resident) but does not change *which thread* any work lands
  on: after P1/R1 everything it would shrink already runs on workers. It is a
  size/VRAM/load-latency project that belongs with the `.amat`/`.aast` import
  pipeline. Honest indirect benefit: smaller worker jobs shorten the window in
  which R3's contention can bite, and shrink staging traffic — welcome, not
  sufficient to sequence it here.
- **Dedicated transfer/copy queue** (was P4) — **stays rejected, with
  evidence.** nvrhi emits no queue-family ownership transfers
  (`VK_QUEUE_FAMILY_IGNORED` everywhere, exclusive sharing), so copies on a
  different-family queue are undefined per Vulkan spec §7.7 — latent
  cross-vendor corruption. A same-family second queue isn't portable (AMD
  commonly exposes one graphics queue). And the stutter was never GPU-side:
  submission is one mutex-guarded `vkQueueSubmit` per frame. Revisit only if
  GPU frame time measurably rises during streaming after R1-R3.

## How to measure

- **Ship build only** (`make gs`). The debug build overstates glm/loop-heavy
  costs ~20× (see the `-O0` caveat above) and will misrank every item here.
- **Metric: worst-frame / p99 frame time during a seamless preload**, compared
  against the same scene idle. Not average pump cost — a 60 s load of 2 ms
  pumps is a success; one 20 ms frame is the failure, however cheap the load
  was on average.
- The pump diagnostic (AssetCache.cpp:876-885, ≥2 ms threshold) localizes any
  spike: `mesh` high ⇒ R1/R2 regression; `mat` high ⇒ descriptor writes (many
  distinct textures — revisit inventory row 4); `flush` high ⇒ submit; all low
  but total high ⇒ preemption ⇒ R3.
- After R1: verify `mesh 1x` time for car_lod drops from ~1.9 ms to µs in
  ship, and that no pump line scales with the asset being published.
- Items marked *unmeasured* (grow cost, sweep cost, post-R1 contention) get a
  measurement before their fix gets built — this doc has already had one
  priority inverted by a `-O0` artifact.
- GPU output correctness needs eyes-on after each step (correct
  meshes/materials, flat frame graph); Vulkan validation layers in the debug
  build are the threading safety net.

## Key file references

- `modules/Render/src/AssetCache.cpp` — publish queue, `PumpPublishes`
  (:809-886), `PublishMesh` (:714), `PublishMaterial` (:722), `FlushUploads`
  (:784), worker record `DecodeAndRecordMaterialChannels` (:627), `.amat`
  main-thread parse (:519-527), `kUploadChunkSize` (:84).
- `modules/Render/include/Assisi/Render/GeometryArena.hpp` — `Allocate` (:77),
  `Grow` (:144), initial capacities (:51-52).
- `modules/Render/include/Assisi/Render/MeshBuffer.hpp` — `Upload` (:54).
- `modules/Render/src/Texture.cpp` — `CreateImage`/`RecordMips`/`UploadDecoded`.
- `modules/App/src/LevelRuntime.cpp:62` — `UpgradeStreamingAssets`;
  `modules/Runtime/src/AssetResolve.cpp:37` — the sweep body.
- `modules/Editor/src/EditorApp.cpp:716-745` — pump call, budgets, per-world
  sweep. `modules/App/src/World.cpp:223-288` — `PumpPendingLoad`.
- `modules/App/src/Application.cpp:342` — `DrainMain` safe point.
- `modules/Core/include/Assisi/Core/JobSystem.hpp:33` — `Pool::IO` "not yet
  built"; `modules/Core/src/JobSystem.cpp:10-18` — worker count.
- `modules/Physics/src/PhysicsWorld.cpp:139-141` — Jolt pool sizing.
- `modules/Render/src/VulkanContext.cpp:996-997` — swapchain semaphores (why
  submits stay on main).
- Vendored nvrhi: `out/build/gcc-debug/_deps/nvrhi-src/src/vulkan/` —
  vulkan-buffer.cpp (:48-49 usage flags, :225 copyBuffer, :444 writeBuffer,
  :578 mapBuffer), vulkan-queue.cpp (:83, :122-129), vulkan-texture.cpp (:474),
  vulkan-device.cpp (:650), vulkan-allocator.cpp (:28-45),
  vulkan-upload.cpp (:49-57), vulkan-backend.h (:1352).

Sources: nvrhi ProgrammingGuide; Vulkan spec §7.7; Unity
`Application.backgroundLoadingPriority`; UE `s.AsyncLoadingTimeLimit` /
AsyncLoadingThread; Donut TextureCache.
