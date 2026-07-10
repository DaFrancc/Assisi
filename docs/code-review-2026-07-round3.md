# Codebase review, round 3 — 2026-07-10

Third full first-party review (all of `modules/`, `apps/sandbox`, shaders,
`tools/reflectgen`, build system, tests), done independently and *then*
compared against rounds 1 and 2. Verified that round 2's burn-down is real:
the two-pass serializer load, `SparseSet::Add` stale-slot rejection, the
transform cycle guard, frames-in-flight replacing the per-frame
`waitForIdle`, validation layers, the depth-format fallback chain,
`MeshPass::InvalidateBindingSets` actually called from `LoadLevel`, the
light-vector member reuse, and the `main.cpp` split are all in the code.
Nothing checked off in round 2 was fake.

Scored **7.5/10** — up from round 2's 7. The half point is round 2's own
"path to 8" (its bug list) being completed. The other half is withheld
because new latent bugs entered *with* the new features — violating round
2's "no new promissory notes" principle — and the hygiene layer (README,
dependency pinning, dead deps) regressed while the code layer improved.

Checkboxes so this can be burned down like the previous two docs.

## Correctness bugs (real, not taste)

- [x] **`Scene` is copyable and copying it double-frees every component
  pool.** `Scene.hpp` manually `new`s pools (`GetOrCreatePool`,
  `Scene.hpp:196`) and `delete`s them in `~Scene`, but declares no copy/move
  operations. One accidental `Scene copy = scene;` and both destructors
  delete the same pools. `Registry` has the same hole: default copy
  duplicates its raw `_pools` pointers. Default move is also broken — the
  pools' registration back-pointers aren't re-seated. Fix: delete copy *and*
  move on both `Scene` and `Registry` (or implement move properly with
  re-registration). This is a rule-of-five violation on the engine's most
  central type.
  *Done (2026-07-10):* copy and move are deleted on both `Scene` and
  `Registry`, with a defaulted default constructor re-added to each (declaring
  the deleted special members otherwise suppresses it). Nothing in the tree
  copied or moved either type — `SceneRegistry` owns scenes through
  `std::unique_ptr<Scene>` — so the deletion is free. The dangerous case was
  specifically `Scene`'s *implicit* copy constructor (its user-declared
  destructor leaves copy generated but move not, so `Scene b = std::move(a)`
  silently selected the shallow copy). Guarded at compile time by
  `static_assert`s on the four traits in `TestScene.cpp` / `TestRegistry.cpp`.

- [ ] **`DebugUI` has the exact disease round 2 cured in `MeshPass`, plus a
  hard capacity wall.** `s_textureIds` (`DebugUI.cpp:41`) is keyed on raw
  `ITexture*` and never evicted — pointer-reuse aliasing and unbounded
  lifetime, the same two hazards `MeshPass::InvalidateBindingSets()` was
  added for. Worse: the descriptor pool is created with `maxSets = 16`
  (`DebugUI.cpp:98`), and every asset-browser thumbnail consumes a set via
  `GetOrCreateTextureId`. A directory with ~15 images exhausts the pool and
  `ImGui_ImplVulkan_AddTexture` starts failing with Vulkan errors. There is
  no eviction API and no growth path. Fix: size the pool realistically or
  grow it, and add an invalidation path mirroring the MeshPass one (called
  wherever a registered texture can be destroyed).

- [x] **`SandboxApp::OnStart` "aborts startup" without aborting anything.**
  If `_scenes.Create("Main")` fails it logs and returns
  (`SandboxApp.cpp:78`), leaving `_scene == nullptr` — then `OnFixedUpdate`
  runs `_physics.SyncTransforms(*_scene)` and `OnUpdate` runs
  `_systems.Run(..., {*_scene, ...})` with no guard. Guaranteed null deref
  on the first frame of the failure path. Fix: `RequestClose()` on failure
  and guard the per-frame hooks on `_scene`, or make scene-creation failure
  fatal in `Initialize()`.
  *Done (2026-07-10):* the failure path now calls `RequestClose()` (Run()'s
  first `ShouldClose()` check then fails and the loop body never runs), and
  `OnFixedUpdate`/`OnUpdate` early-return on a null `_scene` as
  defense-in-depth — matching the guard `OnRender` already had. Both layers,
  since the hooks are public overrides that a future Run() ordering change
  could otherwise re-expose.

- [ ] **`VulkanContext::CreateSwapchainResources` failure paths lie about
  their state.** Once `vkCreateSwapchainKHR` succeeds, the old swapchain and
  its resources are destroyed (`VulkanContext.cpp:464-468`) — but the
  function can still fail *after* that (render-finished semaphore creation,
  unknown swapchain-format mapping, no supported depth format). `Resize()`'s
  comment claims "the previous swapchain is left intact, so BeginFrame()
  keeps working" — only true for the create-fail path. In the late-failure
  paths `_swapchain` is the new handle while `_swapchainTextures` /
  `_framebuffers` are empty, and `BeginFrame` will index into empty vectors.
  Fix: either make the late steps infallible-or-fatal, or leave the object
  in a consistent "no swapchain" state (`_swapchain = VK_NULL_HANDLE`) on
  any partial failure so `BeginFrame`'s existing guard catches it. Correct
  the `Resize()` comment either way.

- [ ] **`SparseSet::Add(NullEntity)` is UB.** `entity.index + 1` with
  `index == UINT32_MAX` wraps to 0, so `_sparse.resize(0, Invalid)`
  *shrinks* the array and the subsequent `_sparse[entity.index]` write is
  wildly out of bounds (`SparseSet.hpp:55-58`). `Scene::Add` is protected by
  its `IsAlive` gate, but `SparseSet` is public and its own docs position
  `Add`'s rejection logic as "defense-in-depth for direct pool users" — that
  defense has a hole exactly at the sentinel value. Fix: reject (or assert
  on) `index == UINT32_MAX` in `Add`. Land with a unit test, same as the
  previous ECS fixes.

- [ ] **Duplicate system names silently corrupt the dependency graph.**
  `TopoSort` builds `nameToIndex` with `emplace`
  (`SystemRegistry.cpp:33`), so a second system registered under the same
  name is unreachable by `After`/`Before` — all edges bind to the first. No
  error, no log. Fix: reject duplicate names loudly at `Register` time.

- [ ] **`EventQueue::Read` spans have an undocumented invalidation
  contract.** The span points into the `TypedQueue`'s vector
  (`EventQueue.hpp:88-94`); a consumer that pushes an event of the *same
  type* while iterating reallocates it — silent UB. `Scene::Query` got a
  warning box for exactly this class of bug; `EventQueue` didn't. Fix:
  document the contract on `Read` (and consider a debug assert via a
  "reading" flag).

- [ ] **`SystemPhase::_Count` is a reserved identifier.** Underscore +
  uppercase is reserved in *all* scopes (`SystemRegistry.hpp:73`). Works on
  MSVC/GCC/Clang today; still squatting on reserved names. Rename (`Count`,
  `kCount`). `SpotLightGPU::_pad` is legal (lowercase, member scope) but
  worth renaming for consistency while there.

## Architecture

- [ ] **Physics is compiled against the renderer.** `PhysicsWorld.cpp`
  includes `Runtime/Components.hpp` for `TransformComponent`, and that
  header drags in `Render/MeshBuffer.hpp`, `Render/Texture.hpp`, and thus
  nvrhi — into the physics module. `TransformComponent` is the most
  foundational data type in the engine and it lives in a header coupled to
  GPU types. Fix: split `Transform` into its own header (or a lower module);
  the layering violation evaporates and Physics stops rebuilding when a
  render header changes.

- [ ] **`Application` is a framework and a debug tool at once.**
  `DrawOptionsWindow` is ~200 lines of ImGui/ImPlot UI *in the engine base
  class* (`Application.cpp:459-660`), which also forces the App module to
  link ImPlot. AA modes, FPS caps, percentile plots — that's a debug overlay
  component, not `Application`'s business. Every game built on this template
  ships an options window it didn't ask for and can't replace without
  editing the engine. Fix: extract into a `Debug`-side overlay the app opts
  into (fits the template-product model — the template wires it up, the
  engine doesn't hardcode it).

- [ ] **Textures: no sRGB formats, no mipmaps.** Albedo loads as
  `RGBA8_UNORM` and the shader decodes with `pow(2.2)` (`cube_min.frag`) —
  so the *sampler filters in gamma space* (wrong, visibly so on
  high-contrast textures) when `RGBA8_SRGB` would give correct filtering for
  free. No mip generation means every textured surface aliases at distance.
  Also each texture upload creates and blocks on its own command list
  (`Texture.cpp:28-32`) — fine for 5 textures, a stall festival the day a
  real level loads. First thing an outside eye will see on screen; belongs
  on the path to 9.

- [ ] **`PropagateTransforms` allocates a fresh `unordered_map`,
  `unordered_set`, and `std::function` recursion every call**
  (`Hierarchy.cpp:16-30`) — and it runs at least twice per frame (game scene
  + camera scene). Round 2 flagged the map as a perf note; the fix hasn't
  landed and the function has since grown a *second* allocation (the
  cycle-guard set). Hottest CPU path after the query loop itself. Fix:
  member/static scratch buffers, or an iterative pass.

- [ ] **Silent capacity ceilings.** `kMaxBodies = 1024` with
  `CreateAndAddBody`'s return never checked (`PhysicsWorld.cpp:204`) — body
  1025 fails invisibly. `Buffer::Upload` silently drops overflow lights
  (documented, but a warn-once costs nothing). The "works until demo day"
  class. Fix: check the body ID and log; warn-once on light overflow.

- [ ] **Camera-in-its-own-Scene is a smell.** `_cameraScene` instantiates an
  entire ECS world to hold one entity, purely so level loads don't clear the
  camera. A serialization-exclusion mechanism (or not storing the editor
  camera in the ECS at all) is the honest design.

- [ ] **Non-uniform scale silently breaks lighting.** `cube_min.vert`
  documents "assumes uniform scale" for the normal transform — but the
  inspector lets you drag `scale` to non-uniform values freely, producing
  wrong lighting with no warning. Either use the inverse-transpose (cheap:
  computed CPU-side per draw) or surface the constraint.

## Build / hygiene (this is where the rot is)

- [ ] **Unpinned dependencies.** `stb` at `master`, `implot` at `master`,
  ImGui at the moving `docking` branch (`CMakeLists.txt:190-296`). NVRHI is
  carefully pinned to a commit hash with an explanatory comment — the
  standard exists and three deps ignore it. A fresh clone next month builds
  different code. Fix: pin all three to commit hashes.

- [ ] **Assimp is fetched, compiled (the single slowest dep), and linked —
  and zero code uses it.** There is no mesh-file loader ("no mesh-file
  loader exists yet" — `AssetCache.cpp:45`). Every configure of every preset
  pays minutes for a library with no call sites. Delete it until the loader
  exists — re-adding it later is one FetchContent block.

- [ ] **README is lying.** The Render section says "currently OpenGL, in the
  future it may support Vulkan or DirectX" and the Debug section says "GLFW
  + OpenGL3 backend" — the engine is Vulkan/NVRHI and OpenGL is gone. The
  Documentation/Links sections are `<add-docs-link>` placeholders. First
  thing a visitor reads; it describes an engine that no longer exists.

- [ ] **`-ffast-math` in Release while embedding Jolt is a risky default.**
  Jolt's docs warn against fast-math (it relies on IEEE semantics and NaN
  checks). The flag only applies to Assisi TUs, not Jolt's own — but
  engine-side NaN guards (e.g. against degenerate transforms) silently
  become no-ops under it. Make it opt-in per-module rather than a global
  Release default, or document the accepted risk explicitly.

- [ ] **`ShortNames.hpp` relies on a comment for safety.** It injects
  `namespace Core = A::Core;` etc. at global scope with "CPP ONLY!!!" as its
  only enforcement. One careless include in a header and downstream
  conflicts get baffling. Consider a `#ifdef` guard trick (e.g. error if
  included before a known .cpp-only marker) or accept and document the risk.

- [ ] **Doc nit:** `AssetPath.hpp:14` says "128-byte value" —
  `TrivialString<127>` is 127 data + 2 length = 130 bytes with padding.
  Tiny, but this codebase trades on the accuracy of its comments.

## What's good (don't regress it)

Everything rounds 1–2 listed still holds, plus specifically earned since:

- The frames-in-flight design done *correctly* on the first real attempt:
  per-slot event queries, per-image render-finished semaphores (with the
  why-comment), GPU-wait time surfaced separately so CPU frame time stays
  honest.
- The CPU/GPU timing split in `Application::Run` — subtracting pacing sleep
  and GPU-throttle waits before reporting CPU cost — is a measurement setup
  most hobby engines never build. The 1%-low stats likewise.
- `SystemRegistry`'s collapse onto `Phase<Ctx>` removed the duplication
  *and* made the wrong-phase state unrepresentable — the right fix shape.
- The ECS liveness story is now defended at every layer that was attacked in
  rounds 1–2, and the regression tests encode the actual failure sequences.
- `AssetSystem`'s two-mount design with a single escape-checked
  `ResolveUnder` spine, tested on both mounts.
- Comment discipline remains exceptional (Y-flip/winding, push-constant
  budget, opener pipeline, NVRHI feature-enable rationale, semaphore
  lifetime notes).

## Scoring and the path up

**7.5/10 now.** Round 2's path-to-8 (its bug list) was completed — that's
the half point. Withheld: the new promissory notes above (descriptor-pool
wall, unchecked body cap, copyable `Scene`) landed *after* round 2 wrote
"no new promissory notes," and the hygiene layer rotted while the code
improved.

**To 8 — fix this document's bug list.** Items under "Correctness bugs"
plus the pinning/Assimp/README hygiene items. Mechanical; an afternoon or
two.

**To 9 — unchanged from round 2, plus one addition.** Enforced contracts
(debug asserts, not doc warnings — `Scene::Query` structural-change counter,
`EventQueue` read-while-push), CI actually running the gates that exist as
config files, a second consumer for the App/reflection/SystemRegistry
layers, and (new) **a real texture pipeline** — sRGB formats, mip
generation, batched uploads — because it's the first visible quality gap.

**To 10 — same asymptote as round 2:** shipped scar tissue, measured
performance work, an explicit threading model, owned failure modes. The
score follows the debt ledger, not the feature count.
