# GPU-driven rendering — design notes (future)

Captured 2026-07-12. **Almost nothing here is built yet** — the only piece that
exists is coarse CPU frustum culling (see "Current state" at the end). This
records the intended endpoint for draw submission so the current per-object CPU
draw loop isn't mistaken for the target, and so the round-4 review's "draw
submission has no culling, sorting, or instancing" scaling-cliff item is
understood as *the first step of this*, not a standalone TODO.

## Design stance (the non-goals matter as much as the goals)

This engine is a **game runtime**, and it optimizes for the **instance-count**
axis — many individual objects — under the assumption of **well-authored
assets**. Two deliberate rejections follow, and they are load-bearing:

- **No Nanite / virtualized geometry / meshlet culling.** Virtualized geometry
  has a fixed baseline cost (visibility buffer, software raster for micro-tris,
  cluster hierarchy) that only pays off at extreme geometric density, and it
  encourages million-triangle source meshes. An engine that assumes sane poly
  counts runs a leaner, more direct pipeline that is faster in the common case
  and far simpler to build and debug. We accept the tradeoff that a single dense
  mesh filling the screen submits *all* its triangles (object-level culling is
  all-or-nothing per mesh) — that is the artist's problem to solve with a better
  mesh, not the engine's to paper over.

- **No LOD generation.** LODs are authored in external DCC tools
  (Blender Decimate / Simplygon / InstaLOD). The engine's LOD responsibility is
  exactly two runtime jobs: **import** a level-of-detail chain by naming
  convention (`Mesh_LOD0..LODn`) as index ranges in the mesh asset, and
  **select** the level by projected screen size in the cull pass. Everything
  upstream (decimation quality, normal-map baking, hero-asset hand-work) lives
  in the tools, where it belongs.

The engine still *renders* bad meshes; it just isn't tuned for them, and that is
by design — assuming good practices yields a better average result than trying
to accept everything.

## The two scaling axes (and which one we target)

| Axis | Unit | Bottleneck | Solution |
| --- | --- | --- | --- |
| **Instance count** | meshes / draws | CPU per-object work, draw-call overhead | indirect draws + instancing + GPU culling (this doc) |
| **Triangle count** | primitives | GPU vertex/geometry throughput, overdraw | authored LODs + occlusion culling |

We target the instance axis: hundreds of thousands to low millions of instances.
The triangle axis is handled by authored discrete LODs, **not** by sub-object
(meshlet) culling. Frustum/occlusion culling helps both axes — culling an object
removes its draw *and* its triangles — but only at whole-object granularity.

## The target architecture — GPU-driven rendering

The CPU stops iterating the ECS and issuing draws. Per frame:

1. **Shared geometry arenas.** One large vertex buffer + one large index buffer;
   each mesh (and each of its LODs) is a `(vertexOffset, indexOffset,
   indexCount)` range. This is what lets a single draw reference any mesh — the
   precondition for indirect draws.
2. **Instance data in a GPU structured buffer** (SoA): transform (or a transform
   index), bounding sphere + AABB, mesh id, LOD info, material id. The ECS is the
   source of truth; this buffer is uploaded/updated, ideally incrementally with
   dirty tracking.
3. **A compute pass culls every instance** (frustum + occlusion), picks its LOD
   by projected screen size, and compacts the survivors into a list. Brute-force
   per-instance in compute is trivially parallel and cheaper than maintaining a
   CPU hierarchy — a BVH's real home is ray tracing / streaming / physics, **not**
   draw-frustum culling.
4. **A compute pass builds an indirect draw-command buffer**, bucketed by
   pipeline/material.
5. **The CPU issues a handful of `drawIndexedIndirectCount` calls** — one per
   material bucket. Ten thousand objects or one, CPU submission cost is flat.

Instancing and sorting fall out for free: identical `(mesh, material)` pairs
collapse into one indirect draw with an instance count; unique objects still
submit in a few multi-draw-indirect calls; buckets are ordered by material for
state coherence and front-to-back for early-Z.

## Culling method — two-phase HZB occlusion

Frustum culling alone does not scale in dense scenes (it can't cull the character
behind the wall). The target is **two-phase occlusion culling with a
hierarchical Z-buffer**, the modern latency-free standard (UE5, id Tech):

- **Phase 1:** cull all instances against the frustum + *last frame's* HZB
  (temporal coherence: what was visible probably still is). Draw survivors.
- Build a fresh HZB (a depth mip pyramid) from that pass.
- **Phase 2:** re-test only the Phase-1 rejects against the *new* HZB — catches
  geometry disoccluded by camera motion. Draw the newly visible.

Bounding volumes: **sphere for the cheap frustum reject; screen-space AABB
(projected) for the HZB depth test.** Compute both per-mesh once at upload and
store on the mesh. The "large flat floor never culls" limitation of a lone sphere
test (its isotropic radius swallows the play area) is dissolved by occlusion
culling, not by a tighter frustum bound.

## What this subsumes from the reviews

This is a render-backend rewrite, not a feature bolt-on, and it *dissolves*
several existing debt items rather than merely addressing them:

- **`MeshPass::_bindingSetCache` unbounded within a level** (round 4) — gone.
  Bindless materials (a descriptor table indexed by a material's texture id)
  replace per-albedo binding sets entirely; there is no cache to bound.
- **"Draw submission has no culling, sorting, or instancing"** (round 4) — this
  *is* the answer; all three arrive together.
- **"Single hardcoded pass sequence in `RenderFrame`"** (round 4) — forces the
  pass-ordering abstraction the item anticipates (cull → build → depth → HZB →
  draw).

## NVRHI capability checks (verify before committing to a stage)

NVRHI has the needed primitives, but confirm specifics before relying on them:

- **Compute** — yes (already used by clustered lighting).
- **`drawIndexedIndirect`** — yes. **`drawIndexedIndirectCount`** (count-buffer
  variant) — verify; the underlying `VK_KHR_draw_indirect_count` is core in
  Vulkan 1.2, which the device selector already requires.
- **Bindless** — via NVRHI descriptor tables; confirm the Vulkan backend's
  descriptor-indexing path and the device feature flags.
- **Multi-draw-indirect** count/stride limits on the target hardware.

## Staging — a migration path, not a menu

Every stage leaves a **complete, shippable, correct renderer**; later stages
strictly depend on earlier ones (you can stop anywhere, but not skip ahead). The
payoff is back-loaded: stages 2–3 are invisible plumbing that make the wins at
4–6 *possible* without changing the picture. Stop at the stage your target scale
demands.

| Stage | Deliverable | Unblocks / gets you |
| --- | --- | --- |
| **1** | Per-mesh AABB beside the sphere | occlusion bounds; cheap frustum tightening. Standalone — additive to today's CPU cull, needs nothing else. Good to ~thousands of objects. |
| **2** | Shared vertex/index geometry arena; meshes → ranges | indirect draws referencing any mesh |
| **3** | Per-instance GPU buffer + **bindless** materials | kills the binding-set cache; one draw samples any texture |
| **4** | CPU-built **indirect** draws + instancing/batching | proves the submission rewrite with cull logic already trusted. Good to ~tens of thousands. |
| **5** | Move frustum cull to a **compute** pass → indirect count; LOD selection rides along | CPU per-object cost gone |
| **6** | **HZB two-phase occlusion** | the real dense-scene unlock. Good to hundreds of thousands / dense occlusion. |

Note the two kinds of change mixed here: stages 2–3 are **structural** (data
layout), stages 4–6 are **runtime passes**. That is why the stages feel like both
a pipeline (ordered) and layers (each shippable) — they are a build order toward
one runtime pipeline, not runtime layers you toggle.

**Caveat carried from the discussion:** build to the scale actually targeted and
measure at each stage. Stages 5–6 are hard to debug (draws vanish into buffers,
sync hazards, temporal edge cases), so the indirect/bindless plumbing must be
rock-solid *before* individual draws stop being visible. Even AAA engines built
this incrementally.

## Current state

Stages 1–4 (this doc's numbering = `mesh-material-architecture.md`'s B–E) are
built. The mapping and per-stage status live in that doc's §9 rollout table;
briefly:

- **Stage 1 (B):** coarse CPU frustum culling — `Render::BoundingSphere` +
  `ComputeBoundingSphere`/`TransformedBoundingSphere`, `Render::Frustum`
  (Gribb–Hartmann, zero-to-one depth), plus the per-mesh AABB refine. Runtime
  toggle + drawn/culled counter in the F12 overlay (`SetFrustumCulling`,
  `DrawStats`) for A/B measurement.
- **Stage 2 (C):** shared `GeometryArena` — one vertex + one index buffer, meshes
  are ranges (base offsets); the single-buffer precondition for indirect draws.
- **Stage 3 (D):** per-instance GPU buffer (world matrix + material id, indexed by
  gl_InstanceIndex) + bindless material textures + material table; one global
  binding set, no per-draw binds.
- **Stage 4 (E):** CPU-built indirect draws — `MeshPass::Submit` coalesces
  consecutive same-(mesh,submesh) items into instanced
  `DrawIndexedIndirectArguments` and multi-draws the frame with one
  `drawIndexedIndirect` per arena buffer-group (~1). Device requires
  `multiDrawIndirect` + `drawIndirectFirstInstance`. Overlay shows
  instances→batches→indirect-calls; the "Sort Draws" A/B now measures instancing.

**Stage 5 (F) is being built in two steps; stage 6 (G) is unbuilt:**

- **F1 (done, GPU-verified):** GPU compute *frustum* cull feeding an indirect-count
  buffer, **LOD0 only**, **one indirect command per surviving submesh** (no
  cross-object coalescing), behind a **"GPU Cull" A/B toggle** that keeps the
  stage-4 (E) CPU path as the pixel-exact reference. Proved the "draws are built
  on the GPU and vanish into buffers" plumbing against a trusted image before
  anything relies on it. A GPU→CPU draw-count readback (a small CPU-readable ring,
  a few frames stale) surfaces the survivor count in the overlay — without it
  culling is invisible and unverifiable, so it belongs in F1, not F2. Two bring-up
  lessons: GPU-written indirect/count buffers need `keepInitialState=true` +
  `initialState=UnorderedAccess` (that *seeds* NVRHI's tracked state so it barriers
  UAV→IndirectArgument; `false` leaves the state Unknown and crashes in
  `requireBufferState`), and the overlay must report the readback survivor count,
  not the capacity, or culling looks inert.
- **F2 (deferred, once F1 is established):** GPU-side **instance coalescing**
  (restores stage-4's batch collapse, which F1 gives up — so `batches` == drawn on
  the F1 path today), **projected-screen-size LOD selection** (the "LOD selection
  rides along" half), and a **dirty-tracked ECS→GPU object mirror** so the
  per-frame CPU gather disappears too — the point at which "CPU per-object cost
  gone" is fully true.
- **G:** two-phase HZB occlusion.

Revisit F2/G when instance counts approach the point where the CPU per-object
extract loop becomes the bottleneck.
