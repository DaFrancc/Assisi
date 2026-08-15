# Asset streaming — design notes (deferred)

Captured 2026-07-10. **Nothing here is built yet.** This records where the asset
pipeline should go so the current "load every texture in a level up front"
placeholder isn't mistaken for the intended design, and so the deferred
"texture upload batching" review item is understood as *subsumed by streaming*
rather than a standalone TODO.

## The problem with today's model

Level load resolves assets **eagerly and lazily**: `SandboxLevels.cpp` walks
every entity and calls `AssetCache::ResolveTexture` / `ResolveMesh`, each of
which uploads immediately on first use (`Texture::UploadTexture` opens its own
command list, writes, closes, and submits — one GPU submission per texture).

This works only because levels are tiny. It scales badly the moment real assets
exist:

- **Per-texture submissions.** N textures = N `executeCommandList` calls, each
  with driver/queue/fence overhead. ("A stall festival the day a real level
  loads" — code-review round 3.)
- **Everything loads, whether or not it's near the player.** No notion of a
  working set; a large world would try to resident-load all of it at once.
- **Synchronous on the main thread.** The frame stalls for the whole load; no
  loading screen is even possible, because the thread that would draw it is the
  one blocked doing the load.

## The intended model — a streaming asset system

Four layers, roughly in dependency order:

1. **Async load + background GPU upload.**
   Decode images and generate mips on worker threads; push uploads through a
   transfer/copy queue with staging buffers so the main thread never blocks.
   NVRHI supports this (separate command lists, a dedicated copy queue). This is
   the foundation — it's what makes a loading screen possible at all (the main
   thread renders UI while workers fill residency).

2. **Loading screen.**
   A coarse gate for the initial / full-level load: block gameplay until a
   *required working set* is resident, show progress. Simple once (1) exists —
   it's just the main thread rendering a UI while the async loader runs.

3. **Spatial streaming (chunks / cells).**
   Partition the world; keep resident only the assets referenced by nearby
   chunks. As the player moves, prefetch incoming chunks and evict distant ones.
   Needs a **residency/refcount table** and a **memory budget** so it doesn't
   thrash. **This is where upload batching lives** — the streaming loader
   naturally batches whatever it uploads per tick, so the standalone
   "batch N textures into one command list" optimization is never written on its
   own; doing it now would be throwaway work.

4. **Mip / LOD streaming** (AAA far end, probably out of scope).
   Stream only the mip levels a surface needs at its current distance rather than
   the whole texture — virtual-texturing territory. Same machinery as (3) taken
   further. Noted for completeness; likely never needed at this engine's scope.

## Geometry-arena residency & compaction (a layer-3 concern)

The shared geometry arena (GPU-driven stage C) is a **bump allocator**: meshes
append, `Reset()` frees everything at once. That's exactly right while the only
free is a wholesale level unload (`AssetCache::Clear()` today). Spatial streaming
(layer 3) breaks that assumption — the player walks into city A (a burst of mesh
uploads), then leaves (most of them freed) — so the arena needs **per-mesh free
+ reclamation** without falling back to a fragmenting free-list. The chosen model
is a **compacting arena** (semi-space), not in-place reuse.

**Free is bookkeeping only.** `Free(range)` marks a range dead and adds its size
to a `freedBytes` counter. No hole reuse in place — holes are reclaimed in bulk
by compaction. So the allocator stays a bump cursor plus a dead-bytes tally and
the live-mesh registry; no best-fit search, no coalescing.

**Trigger (hysteresis).** Compact when either `freedBytes` exceeds a fraction of
the arena **or** an absolute floor (~20 MB), **or** when an allocation can't fit
despite enough total free space (fragmentation *blocking* an alloc is the real
forcing function). Add a floor so a near-full live set doesn't trigger churn for
little gain.

**Mechanism — semi-space, fence-gated (NOT a lock).** The hazard in moving live
data is that the **reader is the GPU**, executing command buffers asynchronously;
a CPU read-write lock can't gate GPU execution (releasing it after *recording* a
draw says nothing about when the GPU *executes* the read). So:

1. Allocate a fresh arena buffer.
2. Copy only the **live** meshes into it, densely, on the copy queue (the async
   part). You write a buffer nobody reads and read a buffer nobody writes — the
   race is designed out, not locked around.
3. **Fence-gate the swap:** the new buffer becomes current only after the copy
   completes (semaphore orders copy-queue → graphics-queue). The old buffer goes
   on a **deferred-free queue** tagged with the swap's frame/fence value and is
   released only once every in-flight frame that referenced it has retired.

Transient cost is ~2× that arena's memory during the copy, and you copy all live
data (not just holes) — acceptable, especially per-arena (below).

**Relocation fixup.** When a mesh moves, only its `MeshBuffer` base offsets
(`vertexBase`/`indexBase`) change, and because *nothing caches raw offsets* —
`MeshPass` reads them through `MeshBuffer` every frame — the next frame picks up
the new location for free. Apply the relocations + buffer swap at a **frame
boundary** (or double-buffer the offset table and flip a pointer) so the render
thread never sees a half-updated table; that keeps the hot path lock-free.

**Interaction with stages E/F (must be co-designed).** Once per-instance data
(E) and indirect draw commands (F) bake `baseVertex`/`firstIndex` into GPU
buffers, those are a *second* copy of the offsets the CPU record no longer solely
owns. A compaction must then also rebuild/patch those GPU buffers. Doable (they
are rebuilt per-frame-ish anyway) but the compactor and the indirect-draw stage
have to know about each other — flagged here so it isn't a surprise at F.

**Per-arena compaction pairs with format-keyed arenas.** With one arena per
vertex format, you compact one arena at a time: the big static-world arena
(rocks/buildings/trees) churns rarely and stays compact for long stretches; a
churny arena (NPCs entering/leaving) compacts more often but is smaller. Work is
naturally bounded per event instead of one giant world-compaction, and you can
spread a copy across a couple of frames (incremental) to avoid a spike.

**Timing.** This is streaming-era machinery — it needs per-mesh load/unload,
which doesn't exist yet. Stage C ships the plain growable bump arena + `Reset()`
only. What C *does* guarantee is the seams that make this a drop-in later: the
arena owns a **swappable buffer handle** (the same mechanism growth already uses),
and `MeshBuffer` indirects through the arena rather than holding a raw handle, so
growth, compaction, and multi-arena are all transparent to the draw loop.

## Prerequisites (the "systems that don't exist yet")

- **Mesh-file loader.** Until real assets exist, levels stay tiny and none of
  this bites. (Also the trigger to finally use the fetched-but-unused Assimp —
  see the round-3 doc.)
- **Job / threading system.** Async is impossible without one — and building it
  forces the explicit threading model the reviews keep naming as a path-to-10
  item.
- **Spatial partition** for the world (chunks / cells) for layer 3.

## Sequencing

    mesh-file loader
        → job/threading system
            → async loader + loading screen   (layers 1–2)
                → chunk streaming              (layer 3, batching rides along here)

Texture upload batching is **not** a near-term task on its own. When the async
loader is built, batching falls out of it for free. Recorded here so the intent
survives; revisit when the mesh loader and a job system exist.
