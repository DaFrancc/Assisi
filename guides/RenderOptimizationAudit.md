# Render optimization audit (2026-09-06)

A read-only audit of the render path for GPU (primary) and CPU (secondary)
optimization opportunities. Nothing here has been implemented or measured; each
item is what the code alone says, tagged with how certain the payoff is and how
much work it is. Line numbers are as of commit cbc6d8c on `render`.

Defaults that frame everything: CPU cull path (`_gpuCulling = false`), draw sort
on, `AaMode::None`, RGBA16F scene target, two frames in flight, one command list
per frame on one graphics queue. Shadow tier Medium: 4 cascades at 2048 D32 with
PCF 3x3; one 4096 D16 local atlas with 512 faces, PCF 3x3, cache on.

ImGui is out of scope.

Each finding is tagged with its Linear status, cross-referenced 2026-09-06
against the Engine team's tracker. `[tracked: ENG-nnn]` means an open issue
covers it; `[partly tracked: ENG-nnn]` means the issue covers some of it and the
note says what it misses; `[untracked]` means nothing in the tracker covers it.

Summary: 1 finding fully tracked, 9 partly, 13 untracked.

Two findings (GPU 8 and CPU item 6) are not optimizations at all. They are
shipped violations of ENG-140's own definition of done, which closed Done on
2026-09-04 in PR #239 — the merge at the tip of this branch. Those need a
different conversation from the rest of this list.

---

## GPU findings, ranked by expected payoff

### 1. No depth prepass; the sort key buries depth under material
`[partly tracked: ENG-203, ENG-149]` — ENG-203 is the coarse-depth-bucket
alternative below, almost verbatim, plus overdraw instrumentation; it notes
that a pure depth-major key would fight the batcher. The prepass itself lives
inside ENG-149 as "Option B", one fork of an eight-point HZB issue, gated on an
occlusion-heavy scene that does not exist yet and framed only as an HZB source.
A prepass done purely for overdraw has no owner today, and neither issue
mentions the main pass running `depthFunc = Equal`.
- `modules/Render/include/Assisi/Render/DrawItem.hpp:113` packs the key as
  pipeline | material | mesh | depth, depth in the lowest 16 bits. Front-to-back
  only holds inside a run of the same mesh and material.
- `modules/Runtime/src/Renderer.cpp:299` is the only ordering applied. No
  depth-only pass exists anywhere. The GPU cull path is worse: instances land in
  atomic-add order (`assets/shaders/mesh_cull.comp`).
- Every occluded fragment pays the full `mesh.frag`: five bindless fetches,
  BRDF context, cluster fetch, light loops, shadow taps.
- Fix: a Z-prepass reusing the position-only vertex layout the shadow renderer
  already has (`ShadowDepthRenderer.cpp:280`) and the same instance/indirect
  buffers `MeshPass::Submit` produces, with a null pixel shader (masked variant
  for cutouts), then the main pass with `depthFunc = Equal`, depth write off.
- Cheap A/B first: move a coarse depth bucket above material in the key and
  watch the `render/batches` counter for lost coalescing.
- Impact: High (proportional to overdraw). Certain. Effort: Medium.

### 2. Spot lights are cluster-culled as a full-radius sphere at the apex
`[tracked: ENG-217]` — fully covered, and the issue goes further: it adds a
CPU-side tight bounding sphere for half-angles under 45 degrees as a
no-shader-change first step, the real cone test for 45-90 degrees, and a
cluster-occupancy counter to measure it. Nothing below is missing from it.
- `assets/shaders/cluster_cull.comp:197` ("culled by bounding sphere for
  simplicity"). A 30 degree cone fills about 7% of that sphere, so a spot is
  listed in roughly 10x the froxels it touches.
- Each false positive costs the fragment a 64-byte light load, dot,
  inversesqrt, smoothstep and attenuation before it `continue`s
  (`mesh.frag:1278-1312`).
- Fix: tight cone bounding sphere (half-angle <= 45 deg: centre
  `pos + dir * r / (2 cos t)`, radius `r / (2 cos t)`; else centre
  `pos + dir * r cos t`, radius `r sin t`) plus a cone-vs-AABB test. The
  `SpotLight` record already carries `cos(inner)` and `outerCutoff`; the batch
  needs one more shared vec4 for direction/outer.
- Impact: High where spots are numerous. Certain. Effort: Small.

### 3. Shadow passes re-upload instance/indirect buffers between passes, forcing full GPU drains
`[untracked]` — ENG-200 created the multi-call shape this exploits, but says
nothing about upload batching or barriers.
- `modules/Render/src/ShadowDepthRenderer.cpp:553-573`: every `Render()` call
  writes `_instanceBuffer` and `_indirectBuffer` from offset 0. The sun calls it
  once; the local pass once per 32-view chunk for the bake and again per chunk
  for the dynamic layer (`LocalShadowPass.cpp:472`, from `:595` and `:626`).
- After the sun's draws the buffers sit in ShaderResource / IndirectArgument
  state. NVRHI's next `writeBuffer` needs CopyDest and emits a barrier with
  source stage AllCommands, so the cascade raster must fully retire before the
  atlas upload starts, and again per chunk. `_lighting.Upload`
  (`SceneRenderer.cpp:287` -> `ClusterGrid.cpp:125`) does the same to the
  light buffers right after, so the cluster cull cannot start until the atlas
  raster is retired either.
- Fix: build every view's draw list for the frame (cascades, bake, dynamic)
  before recording any GPU work; upload once into one instance/indirect range,
  sub-allocated per pass by offset; then record all raster. Light-record
  stamping only needs `AllocateTiles` (CPU), so the light upload can move ahead
  of the shadow raster too.
- Impact: High on frames with local shadow bakes or movers (three to seven
  drains today), Medium on resting frames (one drain from the light upload).
  Certain. Effort: Medium to Large.

### 4. Sun cascades are fully re-rasterized every frame
`[partly tracked: ENG-201]` — that issue covers "stop re-rendering unchanged
cascades" thoroughly, and per-cascade cadence falls out of its drift criterion.
But its cadence is threshold-driven rather than a fixed round-robin, and it
explicitly re-affirms the rejection of fix (b) below (the toroidal static
layer, as VSM territory under ENG-80's standing decision). It also warns the
win nearly vanishes under free-look. Half of what follows is filed; the other
half is filed as rejected.
- `modules/Runtime/src/Renderer.cpp:353` ("every caster is drawn, every time");
  `ShadowPass.cpp:221-239` clears all slices every frame.
- The fit (`ShadowCascades.cpp:139-221`) is sphere-bounded and texel-snapped,
  so cascade content only shifts by whole texels as the camera moves. The far
  cascade carries the most casters and the least screen detail.
- Fix (a): render cascades 2..N every 2nd/4th frame round-robin, padding the
  ortho box by expected camera travel and keeping the matrix used at render
  time in `FrameConstants` (the shader already reads
  `shadowViewProjection[cascade]`).
- Fix (b): reuse `ShadowCasterMobility` to keep a static layer per cascade,
  toroidally scrolled by the snapped delta, redrawing only movers.
- First: split the `shadows/instances` counter per cascade
  (`ShadowPass.cpp:249-253` sums it) to confirm the far cascades dominate.
- Impact: High when far cascades dominate. Cost certain, fix speculative.
  Effort: (a) Medium, (b) Large.

### 5. Scene color is RGBA16F with alpha never used
`[untracked]` — ENG-135 (Done) is what shipped RGBA16F and is the source of
this finding; nothing proposes narrowing it or notes that alpha is unused.
- `modules/Render/include/Assisi/Render/PostProcess.hpp:47`
  `kSceneColorFormat = RGBA16_FLOAT`. `mesh.frag:1420`, `sky.frag:163` write
  alpha 1; `tonemap.frag:141` passes it through. No blending pipelines exist.
- Fix: `R11G11B10_FLOAT`. Halves scene-color write, MSAA resolve, and tonemap
  read. Only visible risk is blue banding in very dark gradients.
- Impact: Medium (bandwidth). Certain. Effort: Small.

### 6. Cascade array cleared and rendered as four separate passes
`[untracked]` — ENG-200 parameterized the depth renderer per view and never
revisited the pass count.
- `ShadowPass.cpp:87-104` builds one framebuffer per slice; `:231` clears per
  slice. Each clear is endRenderPass + subresource barrier + transfer clear;
  each cascade's `setGraphicsState` ends and begins a dynamic-rendering pass.
  Per frame: 4 clears, ~8 barriers, 4 pass begin/ends. A caster in all four
  cascades is four instance records and four commands.
- Trivial: one `clearDepthStencilTexture(_cascadeTexture, AllSubresources)`.
  Effort Small, impact Low, certain.
- Real: one layered framebuffer over all slices (NVRHI sets layerCount from
  `arraySize`), enable `shaderOutputLayer` in `VulkanContext.cpp:342-360`,
  have `shadow_depth.vert` write `gl_Layer` from a per-instance view index and
  read its matrix from the view table instead of the push constant. All
  cascades draw in one pass; same-geometry runs coalesce across cascades.
  Effort Medium, impact Medium (Low if the depth pass is purely fill-bound).

### 7. Fat vertex format, fetched at full stride by every depth pass
`[untracked]` — the LOD track (ENG-209, ENG-210, ENG-215) is adjacent, but it
reduces depth-pass vertex work by triangle count rather than by vertex format.
The two compose rather than overlap.
- `modules/Geometry/include/Assisi/Geometry/MeshData.hpp:29-36`: 48 B all
  fp32 (pos3, normal3, uv2, tangent4), 32-bit indices. Shadow layouts
  (`ShadowDepthRenderer.cpp:280`, `:335`) read 12 useful bytes per 48-byte
  stride for every cascade and up to 40 atlas faces per frame at Medium.
- Fix: position in its own stream (12 B); normal and tangent as
  `R10G10B10A2_SNORM` (w = sign), UV as `RG16_FLOAT`. Main stride ~28-32 B,
  shadow stride 12 B.
- Also `assets/shaders/mesh.vert:69`: `transpose(inverse(mat3(model)))` per
  vertex. The result is normalized on the next line, so the adjugate (three
  cross products, no determinant, no divide) gives the identical direction. Or
  precompute per instance (`InstanceData` grows from 80 to 128 B).
- Both matter more once a prepass doubles vertex work.
- Impact: Medium for shadow passes. Certain. Effort: Medium (format), Small
  (normal matrix).

### 8. Whole point light re-composited when one face has a mover
`[ENG-140 regression — not an optimization]` — ENG-140's definition of done
states it literally: "A mover entering range dirties **only** the faces whose
frustum contains it." The shipped code computes that mask and throws it away.
The requirement is on record and was accepted as met; the gap has no open
issue. See also CPU item 6, the other half of the same problem.
- `modules/Render/src/LocalShadowCache.cpp:263-285`: `LocalShadowFaceMask`
  computes which faces a mover reaches, then discards it for movers and sets
  only `hasMovers`. `LocalShadowPass.cpp:608-619` copies the cached tile for
  all six faces every frame (each a barrier + transfer) and `:626` submits
  every face as a dynamic target. One walking character under a point light
  costs six tile copies per frame instead of one or two.
- Fix: carry `moverFaces` in `LocalShadowTilePlan`; copy and redraw only those.
- Keep the copy-based composite; a second atlas with `min` in the shader would
  double PCF taps in the main pass.
- Impact: Medium for point-light-heavy scenes with movers. Certain. Effort:
  Small.

### 9. Shader variant bloat and unoptimized SPIR-V
`[untracked]`
- `mesh.frag` carries eight shadow kernels (Point / PCF3 / PCF5 / Vogel, for
  sun at `:676-751` and local at `:980-1007`) and nine debug paths
  (`:1123-1140`, `:1342`, `:1358-1372`, `:1389-1414`) selected by uniform
  branches. Branches are cheap, but register allocation is for the worst path
  and the instruction footprint is the whole file.
- Fix: NVRHI exposes `createShaderSpecialization`; make sun filter, local
  filter and "debug views" specialization constants, or add `-D` variants via
  `assisi_sandbox_compile_shader` (`apps/sandbox/CMakeLists.txt:139-186`).
  Move the debug views under an editor-only define.
- `CMakeLists.txt:676` sets `ENABLE_OPT OFF` and the compile line
  (`apps/sandbox/CMakeLists.txt:152`) passes no `-Os`, so SPIR-V is emitted
  unoptimized. Drivers re-optimize, so runtime impact is Low, but it blocks
  dead-path removal without spec constants.
- Impact: Medium (occupancy; speculative), Certain (code size). Effort:
  Small to Medium.

### 10. Smaller certain wins
`[untracked except where noted]` — ENG-180 already covers the cull dispatch
underfill (it names the occupancy figure) and the per-batch light re-transform,
though not the grid resolution as a knob nor the double test. ENG-145 mentions
Vogel-disk PCF only as where PCSS would land, never the per-tap recompute.
ENG-204 discusses FXAA and SMAA at length without mentioning the luma-in-alpha
trick. Everything else below is unfiled.
- Vogel disk (`mesh.frag:591-593`, `:985-996`) recomputes sqrt + sincos per
  tap. Only the per-pixel rotation varies: precompute `vec2 kVogel[16]` and
  rotate by one per-pixel (cos, sin). 1 sincos per fragment instead of 16.
- PCF keeps tapping when the kernel collapses: `mesh.frag:702-705` derives a
  step that near contact or in far cascades falls well under a texel, so all
  9 or 25 taps land in one 2x2 footprint. Add
  `if (step * resolution < 0.5) return ShadowTap(...)`. `NearestBlockerDepth`
  and `ProbeCascade` also call `textureSize()` per fragment for a value
  already in frame constants. Speculative on visuals; check.
- Sky shader (`sky.frag:105-116`, `:149`) evaluates air extinction, sun air
  mass (acos + pow + exp), beam and ground per pixel for values fixed per
  frame. Move into `SkyConstants`.
- Sky constant buffer is non-volatile (`SkyPass.cpp:104`); `writeBuffer` on it
  ends the scene render pass between the mesh draws and the sky. Create it
  with `isVolatile = true, maxVersions >= kFramesInFlight`. No volatile
  buffers are used anywhere today.
- Scene color clear (`Application.cpp:884`) is a full-resolution transfer
  write that is entirely overwritten whenever the sky draws (depth Equal 1.0
  covers every uncovered pixel). Skip it when `sky.status == Ready`.
- Depth format order in `VulkanContext.hpp:161` is D24S8 first; stencil is
  never used. Prefer D32 (D24S8 is emulated on AMD).
- Sun map defaults to D32 (`ShadowSettings.hpp:234`). Orthographic ranges
  quantize to ~1 mm at 16 bits, an order of magnitude under the constant
  bias; the local atlas already uses D16. D16 halves depth bandwidth. Verify
  grazing far geometry visually.
- One sampler at device-max anisotropy (`MeshPass.cpp:221-229`) serves all
  five material fetches. Metal/rough, AO and emissive rarely need it; a second
  sampler at 1-2x, or a cap at 8x.
- Cluster grid 16x9x24 (`ClusterGrid.hpp:110-112`) is 120 px tiles at 1080p;
  the cull dispatch is 54 workgroups and underfills the GPU. Doubling XY is
  nearly free on the cull side and halves lights-per-fragment in dense scenes.
  Needs a light-count histogram to confirm.
- `cluster_cull.comp` re-transforms every light into view space per batch per
  pass (`:148`, `:182`, `:206`, `:237`) and runs `sphereVsAABB` twice per
  light per cluster (count then write). Upload view-space positions; keep a
  per-thread bitmask from the count pass.
- `mesh_cull.comp:185-187` (GPU cull path only): one same-address atomicAdd
  per surviving instance for stats. Use subgroup reduction and one atomic per
  wave, or gate on the stats panel.
- `BlankCacheTile` (`LocalShadowPass.cpp:489-514`) leaves the render pass to
  copy a clear tile before each bake. A depth-only fullscreen draw with
  `depthFunc = Always` and a tile viewport does it inside the pass.
- `tonemap.frag:141` writes alpha 1; FXAA then recomputes luma via dot for
  9-20 fetches. Write luma into alpha, read `.a`, and gather the cardinal
  neighbours. Do not merge tonemap into FXAA (it would tone map 9-20x per
  pixel).
- Editor chain (overlays, no FXAA) has an extra full-screen Blit and a
  never-written swapchain depth buffer (`PostProcess.hpp:923-972`,
  `VulkanContext.cpp:810-826`). Shipping chain is already one pass.
- `mesh.frag`: `GeometricNormal()` is recomputed inside all three light loops
  (`:1197`, `:1262`, `:1315`) and `dot(N, L)` twice per light. Hoist.

---

## CPU findings

### Latent bug: exponential light-list duplication in the local caster gather
`[untracked]` — the only correctness defect in this document with no tracker
presence at all.
- `modules/Runtime/src/Renderer.cpp:466-474`: for each additional LOD0
  submesh, the code inserts `reached[firstReach, end)` back into `reached`.
  The end grows every iteration, so a mesh with S submeshes reaching L lights
  appends `2^(S-1) * L` entries, and rows 2.. carry duplicated light ids. Those
  flow into the `index.start` counting and fill passes, so the same caster is
  submitted several times into one light's shadow view.
- It is also a `vector::insert` with iterators into the same vector, which is
  undefined behaviour if the insert reallocates.
- Fix: capture the row length once and copy a fixed range (or push_back in a
  loop after a reserve). No test covers this function.
- Impact: High (correctness plus unbounded allocation). Certain. Effort: Small.

### Ranked
1. `Renderer.cpp:216`: the draw item vector is a fresh local every frame with
   no reserve; `DrawItem` is 96 B and `std::sort` moves whole structs. Reuse a
   member, sort (key, index) pairs. High, certain, small.
   `[partly tracked: ENG-181]` — that issue owns the per-frame vector and
   prescribes a caller-owned buffer threaded through `DrawSceneParams`. It does
   not mention the missing reserve or shrinking the sorted element.
2. The mesh set is walked three to four times per frame: draw extract
   (`Renderer.cpp:222`), sun gather (`:327`), local gather (`:416`), icons
   (`SceneRenderer.cpp:816`), each recomputing `TransformedBoundingSphere`
   (`Bounds.hpp:50-58`, three sqrt where one suffices on the max squared
   length). One extraction pass producing (entity, worldMatrix, worldSphere,
   mesh, materials, castsShadows) into a reused SoA buffer would feed all of
   them. High at scale, certain, large.
   `[partly tracked: ENG-148]` — that issue removes the draw-extract walk for
   static objects, on the GPU cull path only; ENG-214 (Done) removed the
   per-(cascade x class) sweep. Neither names the three-to-four walks per frame
   nor sharing one world sphere across them.
3. `GatherLocalShadowCasters` allocates five scratch vectors per frame
   (`Renderer.cpp:412`, `:487`, `:508`, `:510`) and the final `swap` hands the
   persistent caster array a fresh buffer every frame, destroying its
   capacity. Make them members. High with local shadows, certain, medium.
   `[untracked]` — ENG-181 is the same class of finding but scoped explicitly
   to `DrawScene`.
4. `Hierarchy.cpp:36-140`: transform propagation visits every entity every
   frame (the recompute is skipped, the visit is not) with about six guarded
   static pool lookups per entity via `ComponentIdOf<T>()`. Hoist the pools
   before the loop and pass the yielded Transform in (small); true
   incrementality needs a child index on Parent attach/detach (large).
   `[untracked]` — ENG-177 touches `PropagateTransforms` only for the
   stale-worldMatrix-on-detach correctness bug.
5. Shadow caster sort comparator (`Renderer.cpp:29-38`) recomputes the
   pipeline class from bools at offset ~116 while `geometryKey` is at 0, and
   the sun sort moves 136-byte structs. Fold the class into the high bits of
   `geometryKey` at emit time; sort an index permutation. Medium, small.
   `[untracked]` — ENG-214 requires preserving the class-major order but says
   nothing about the comparator's cost or the element size.
6. `LocalShadowCache.cpp:136-198`: `NoteBaked` inserts every still caster into
   the mobility map and `Update` walks the whole map each frame to find the
   dynamic ones. Keep a flat baked-pose array plus a small dynamic list.
   `[ENG-140 regression — not an optimization]` — that issue requires in bold:
   "Iterate entities whose tick changed this frame ... and **never** casters x
   lights. A fully still frame must skip the caster gather entirely." The
   shipped mobility map violates it. See also GPU finding 8.
7. `ClusterGrid.cpp:125-127`: light buffers fully rewritten each frame
   regardless of change. Dirty-flag after the shadow-view stamping, not inside
   `Gather`. Low to medium, medium effort.
   `[untracked]` — ENG-148 is the identical pattern for objects and is the
   template to copy, but lights are explicitly out of its scope.
8. `SceneRenderer.cpp:564-576`: winner-to-light resolution is a linear search
   per winner over the light pool. Carry an index on the assignment.
   `[untracked]`
9. `MeshCuller.cpp:85` (GPU path): `unordered_map` probe per instance per
   frame. Memoize the last (meshKey, index).
   `[partly tracked: ENG-148]` — the probe lives inside the per-frame
   `AddInstance` path that issue deletes, so it dissolves as a side effect.
   ENG-148 never names it, so a partial landing leaves it unmentioned.
10. `SceneRenderer.cpp:842` (editor only): `IsIconSuppressed` is a linear find
    inside two per-entity loops, and the list holds every collider entity.
    Sort + binary search, or a bitset by entity index.
    `[untracked]` — ENG-156 and ENG-157 touch the same members, but only as
    unbounded-growth leaks on early-return paths.
11. `Renderer.cpp:256`: redundant world-centre transform; it is
    `worldSphere.center` already computed when culling is on.
    `[untracked]` — ENG-209 notes the sphere and view distance are already in
    hand, but only to reuse them for LOD selection.
12. `AssetResolve.cpp:21-33`: while streaming, every material slot is
    re-resolved per frame through a 130-byte `AssetPath` by value and a string
    hash. Memoize id -> Material.
    `[partly tracked: ENG-207 — and it points the other way]` — that issue owns
    this code, and its "Broad" candidate fix accepts "a lookup per slot per
    frame" as the price. Whoever picks up ENG-207 should read this item first
    or they may make it worse. Nothing covers the `AssetPath` copy.

---

## Verified clean (no action)
- No `createBindingSet` or pipeline creation in the steady-state frame path;
  one cached global binding set rebuilt only on handle change.
- No per-draw constant uploads: one frame CB, one sky CB, one instance SSBO,
  one indirect args write per frame. Push constants for compute and post.
- No frame-loop stalls: `waitForIdle` only at shutdown and asset-cache clear;
  BeginFrame waits only on the slot's event query; the GPU cull stats readback
  uses a three-slot ring against two frames in flight.
- `discard` is confined to the `ASSISI_ALPHA_MASK` variants; nothing writes
  `gl_FragDepth`; the opaque pipelines keep early-Z.
- Cluster index is scale/bias FMAs plus one log; BRDF invariants are hoisted
  per fragment; cascade select is branch-free; the sun lookup is skipped past
  the last split, for back faces, and the second cascade only when needed.
- Frustum: six dot products per sphere, positive-vertex AABB test, no
  per-object inversions.
- ECS queries are pool-driven from the smallest pool, contiguous storage, no
  per-entity `Has<>` scans; debug tripwires compile out under NDEBUG.
- No `shared_ptr` or refcount traffic in the draw path; materials are raw
  pointers.
- Light GPU structs are padding-free and asserted.
- Cluster build runs only on projection change.
- Shadow depth pipeline has no fragment stage, back-face culling, slope bias,
  hardware comparison sampler, position-only vertex layout, instanced
  coalescing within a view.

## Tracker status at a glance (2026-09-06)

Fully tracked (1): GPU 2 — ENG-217.

Partly tracked (9): GPU 1 (ENG-203, ENG-149), GPU 4 (ENG-201), GPU 8 and CPU 6
(ENG-140 regressions), two items inside GPU 10 (ENG-180), CPU 1 (ENG-181),
CPU 2 and CPU 9 (ENG-148), CPU 12 (ENG-207).

Untracked (13): GPU 3, 5, 6, 7, 9, most of GPU 10; the CPU duplication bug,
and CPU items 3, 4, 5, 7, 8, 10, 11.

Two structural notes. The depth prepass has no owner: it exists only as one
fork inside ENG-149's HZB work, so if it is worth doing for overdraw alone it
is currently blocked behind a larger issue that may choose the other fork.
And the two ENG-140 items are acceptance criteria that were written, accepted
and not met, which is a different conversation from the rest of this list.

Issues that came back on topical similarity but match nothing here: ENG-146
(SSAO), ENG-141 (shadow diagnostics), ENG-204 (SMAA), ENG-156, ENG-157,
ENG-177. The LOD track (ENG-209, ENG-210, ENG-215) is adjacent to GPU 7 and
composes with it rather than overlapping.

## Suggested order
1. Depth prepass (or the depth-major key A/B), with the normal-matrix fix
   first so the second vertex pass is cheap.
2. Spot cone culling.
3. Shadow upload restructure to remove the drains.
4. R11G11B10F scene target; skip the color clear under a sky; volatile sky
   CB; D32 depth; D16 sun map.
5. Far-cascade update rate, then the layered cascade pass.
6. Point-light face mask; single cascade clear; Vogel table; sky constants.
7. Spec constants and editor-only debug variant.
8. Fix the local-gather duplication bug; reuse the draw list; single mesh
   extraction pass; then the rest of the CPU list as the profiler directs.

## References
- https://therealmjp.github.io/posts/to-earlyz-or-not-to-earlyz/
- https://github.com/WindyDarian/Vulkan-Forward-Plus-Renderer
- https://digitalrune.github.io/DigitalRune-Documentation/html/4f8d2843-e46a-44cf-ba8d-c58fb8d9302d.htm
- https://www.lunarg.com/simplify-spir-v-size-reduction-with-os-option/
