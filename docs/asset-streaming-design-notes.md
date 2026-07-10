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
