# Streaming Upload Performance Plan

Branch: `multi-scene`. Goal: eliminate frame-time stutter caused by streaming GPU
asset uploads during a seamless background level preload (multi-scene S5).

Diagnosis and plan verified against the **vendored nvrhi source** the engine
builds: `out/build/gcc-debug/_deps/nvrhi-src/src/vulkan/`.

## Corrected diagnosis (what actually costs what)

`executeCommandList` is **NOT** GPU-blocking — `Queue::submit` (vulkan-queue.cpp
122-209) is a mutex-guarded `vkQueueSubmit` + timeline-semaphore signal, returns
immediately. The real per-item **main-thread** costs, biggest first:

1. **The staging memcpy inside `writeTexture`** (vulkan-texture.cpp 474-545):
   row-by-row memcpy of pixel data into an upload buffer, on the recording
   thread. A 2048² RGBA8 + mips ≈ 22 MB; a 5-channel material burst is >100 MB of
   main-thread memcpy in one frame. **This is the dominant cost and neither of
   our interim fixes touched it.**
2. **A fresh command list per publish** (`device->createCommandList()` in
   `Texture::UploadDecoded`, `GeometryArena::Allocate`, `WriteMaterialToTable`).
   The `UploadManager` is per-CommandList (vulkan-backend.h:1352), so every
   publish starts with zero chunks → `vkCreateBuffer`+`vkAllocateMemory`+map. GPU
   memory allocation is one of the most expensive Vulkan CPU calls.
3. **One `vkQueueSubmit` per item** — tens–hundreds of µs of driver time each,
   dozens per frame in a burst.
4. GPU-side: many small submits serialize with the frame's render submit (small
   but nonzero).

`writeBuffer` ≤64 KB is inline `vkCmdUpdateBuffer` (cheap; the 96 B material row
is fine); larger buffer writes (mesh vertex data) take the staging path.

### nvrhi threading facts (verified, decisive for P1/P4)

- **Multithreaded command-list recording is supported/intended** (ProgrammingGuide:
  "valid to record multiple command lists concurrently and execute in any order";
  `CommandList::open` → `getOrCreateCommandBuffer` is mutex-guarded, "free-threaded",
  vulkan-queue.cpp:85).
- **`createTexture` is free-threaded** (vkCreateImage + allocator, both internally
  synchronized; nvrhi guards its shared maps). So texture create + writeTexture
  recording **can run on a worker**.
- **Submission (`executeCommandList*`) must stay on the main thread** here:
  `queueWaitForSemaphore`/`queueSignalSemaphore` (vulkan-queue.cpp:126-128, used by
  VulkanContext.cpp:996-997 for swapchain acquire/present) is persistent queue
  state that "will not work well with multi-threaded submission to the same
  queue." Keep `writeDescriptorTable` (bindless registration) on main too.
- `executeCommandLists(lists, count)` (vulkan-device.cpp:650) issues **one**
  `vkQueueSubmit` for an array of lists — batch the executes.
- **nvrhi never emits Vulkan queue-family ownership transfers** — every barrier
  uses `VK_QUEUE_FAMILY_IGNORED`, no resource is `VK_SHARING_MODE_CONCURRENT`. So a
  different-family transfer/copy queue is **spec-undefined** (§7.7) → P4 rejected.
- Upload managers "never shrink their working set" — reclaim staging by releasing
  and recreating the command list once after a big load.

## Current state (already committed on `multi-scene`)

- `2848652` — `Application::SetMainThreadTaskBudget` caps `DrainMain(n)` per frame;
  editor sets 2/frame during a preload. **P0b will retire this for publishes** (it
  counts tasks, not bytes, and throttles all main tasks). Keep the mechanism; stop
  using it as the streaming throttle.
- `f9b0222` — AssetCache concurrency cap (`_maxConcurrentLoads = 3`, `_pendingLoads`
  queue, `PumpLoadQueue`) + sequential channel decode + refactor to named members
  (`StartMeshLoad`/`OnMeshLoaded`, `StartMaterialLoad`/`OnMaterialLoaded`,
  `DecodeMaterialChannels`). **P2 refines this into a byte-weighted cap + low-prio
  IO pool.** Keep the cap for now.

## Plan (priority order)

### P0 — Shared upload command list (one execute/frame)  [High impact, low risk, ~1 day]
Replace the three per-item `createCommandList/open/write/close/execute` sites with
one persistent, reused upload list owned by `AssetCache` (or a small
`GpuUploadQueue` the cache + arena share):
- One `nvrhi::CommandListHandle _uploadList`, created once. First publish of a
  frame `open()`s it; all `writeTexture` / arena `writeBuffer` / material-row
  writes record into it; at the pump point (DrainMain safe point, **before**
  `RenderFrame()` opens the render list) `close()` + **one** `executeCommandList`.
- Correctness is free: same queue, submitted before the frame's render submit →
  draws see the data, nvrhi auto-barriers handle it.
- Reusing the handle reuses the UploadManager's staging chunks → removes the
  per-publish `vkAllocateMemory`. After a preload completes, release+recreate the
  list once to return peak staging memory.
- Pass `CommandListParameters::uploadChunkSize` ~8–16 MB (default 64 KB) so a
  texture burst doesn't fragment into dozens of chunks.
- **GeometryArena grow path** (GeometryArena.hpp 118-145) — two safe options:
  - *Preferred:* reserve capacity once at preload start (sum incoming geometry
    from the import/manifest) → no mid-stream realloc/copy.
  - *Fallback:* record the grow's `copyBuffer(grown,0,old,0,used)` into the **same
    open shared list** (linear ordering + auto-barriers make it safe; old buffer
    kept alive by the list's referencedResources). Never let grow self-execute.
- Sites to change: `Texture::UploadDecoded` (Texture.cpp 216-229),
  `GeometryArena::Allocate`/grow (GeometryArena.hpp 83-90, 118-145),
  `WriteMaterialToTable` / material-table write (AssetCache.cpp ~505-510).
  Precedent: NVIDIA Donut `TextureCache` keeps one reused `m_CommandList`.

### P0b — AssetCache publish queue + time/byte budget  [High impact, low effort]
The `DrainMain(maxTasks)` cap is the wrong layer (counts tasks, throttles all main
work). Budget the integration step itself:
- The `.Then(Pool::Main, …)` continuation becomes O(1): push a
  `PendingPublish { bundle, byteSize }` into an AssetCache-owned queue.
- `AssetCache::PumpPublishes(timeBudgetMs, byteBudget)` runs once/frame at the
  DrainMain safe point: pop-and-publish while `elapsed < timeBudgetMs && bytes <
  byteBudget`, recording into the P0 shared list, then one execute. Carry the
  remainder to next frame. Keep whole-material atomicity (all 5 channels + row in
  one item — never half-textured).
- Defaults: **~2 ms main-thread + ~16 MB per frame during play**; high tier (10+
  ms / unbounded) behind a blocking loading screen. Seamless preload = low tier.
  Precedent: Unity `backgroundLoadingPriority` 2/4/10/50 ms; UE
  `s.AsyncLoadingTimeLimit` 5 ms; Donut `ProcessRenderingThreadCommands(timeLimit)`.
- Retire `SetMainThreadTaskBudget` as the streaming throttle (keep the mechanism
  for other deferred main work if useful, or remove if now unused).

### P1 — Record uploads on workers (memcpys off main)  [High impact, medium effort]
Deletes the remaining main-thread staging memcpy:
- In the mesh/material worker job, after decode: `device->createTexture(desc)`
  (free-threaded), `createCommandList()`, `open()`, `writeTexture()` per mip (big
  memcpys now on the worker), `close()`. Return the **closed** `CommandListHandle`
  + texture handles in the bundle.
- Main-thread publish then only: `executeCommandLists(array)` (one submit for all
  ready lists), `writeDescriptorTable` (bindless slot, main-only, cheap), mint id,
  write 96 B row (into P0 list). Per-material main cost: ms → µs.
- Keep every execute on main (the `queueWaitForSemaphore` landmine).
- Pool the per-load command lists (bounded by the in-flight cap ≈3); drop the pool
  after preload to reclaim staging.
- Move texture dedup to **kick time**: a main-thread `(path,colorSpace) →
  loading/loaded` registry checked before dispatch, so a shared texture decodes +
  records once (today two materials can both decode it, dedup only at publish).
- Meshes: leave arena writes on main inside the P0 batcher (vertex data is small
  next to textures; concurrent arena recording buys little). Budget via P0b.

### P2 — Low-priority IO pool + byte-weighted cap  [Medium]
- Build the `Pool::IO` the JobSystem design already reserves: 1–2 threads at
  **below-normal OS priority** (`pthread_setschedparam`/nice; Windows
  `THREAD_PRIORITY_BELOW_NORMAL`) for decode/import jobs. OS preempts streaming
  instead of frame threads → ends the three-way oversubscription with Jolt's pool
  and main structurally. Precedent: UE AsyncLoadingThread, Unity background thread.
- Make the in-flight cap **byte-weighted** (e.g. ≤64 MB decoded-image bytes in
  flight) rather than count-based (3× 4K vs 3× 256² are very different).
- Keep sequential channel decode (already done; nested parallel-for per asset is
  the anti-pattern).

### P3 — Import-time BC7/KTX2 + pregenerated mips  [High long-term, high effort]
Runtime stbir mip gen + stb decode is the dominant decode cost; RGBA8 is 4–8× the
bytes of block-compressed. Precook BC7/KTX2 with mips (fits the planned
`.amat`/`.aast` import pipeline): deletes the mip pass, ~4× fewer upload bytes,
shrinks every budget above. What UE/Unity/id Tech actually stream.

### P4 — Dedicated transfer/copy queue  [REJECTED]
nvrhi emits no queue-family ownership transfers (`VK_QUEUE_FAMILY_IGNORED`
everywhere, exclusive sharing) → a different-family copy queue is undefined per
Vulkan §7.7 (latent cross-vendor corruption). Same-family extra queue isn't
portable (AMD: one graphics queue). Payoff is small once P0/P1 land (the stutter
is CPU-side). Revisit only if GPU frame time rises during streaming after P0–P2.

## Expected outcome
P0 + P0b: a dozens-of-submits burst frame → one bounded ~2 ms publish slice.
P1: that slice → microseconds + one batched submit. Stutter gone up to the byte
budget, which is then freely tunable.

## Execution order
Do **P0 + P0b** first (contained to AssetCache/Texture/GeometryArena; test the
improvement), then **P1**. P2/P3 later. Cannot self-verify GPU output — needs
eyes-on test after each step (correct meshes/materials, smooth frame graph).

## Key file references
- `modules/Render/src/AssetCache.cpp` — publish sites, `OnMeshLoaded` /
  `OnMaterialLoaded` / `WriteMaterialToTable`; the P0b publish queue lands here.
- `modules/Render/src/Texture.cpp` (`UploadDecoded`, 216-229).
- `modules/Render/include/Assisi/Render/GeometryArena.hpp` (`Upload`/`Allocate`,
  grow 118-145).
- `modules/Render/src/VulkanContext.cpp` (single graphics queue; 316-320, 996-997).
- `modules/App/src/Application.cpp:342` (`_jobs.DrainMain(_mainThreadTaskBudget)`;
  P0b's `PumpPublishes` runs at this safe point).
- Vendored nvrhi: `out/build/gcc-debug/_deps/nvrhi-src/src/vulkan/`
  (vulkan-queue.cpp 85/122-209; vulkan-texture.cpp 264/474-545; vulkan-buffer.cpp
  444-505; vulkan-device.cpp:650; vulkan-backend.h:1352).

Sources: nvrhi ProgrammingGuide; Donut TextureCache.cpp; Unity
`Application.backgroundLoadingPriority`; UE async asset loading; Vulkan spec §7.7.
