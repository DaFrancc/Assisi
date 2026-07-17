# Code review — round 6 (2026-07-17)

Branch: `asset-upgrade` (255 commits, 337 files, ~50k insertions vs merge-base
`9820d45`). Reviewed the whole branch diff across seven domains (ECS/reflection,
Core asset/GUID/jobs, Vulkan platform, mesh/material pipeline, lighting/runtime,
sandbox editor, build/hygiene). Findings below were verified against source and,
where relevant, the vendored NVRHI backend — not pattern-matched.

## Score: 7 / 10

The architecture is coherent across an enormous scope and the hardest parts are
demonstrably correct: frames-in-flight synchronization, the cluster-cull atomic
clamp math, the F2a per-batch reservation scheme, and **every** GPU/CPU struct
layout (the review hunted mismatches and found none) all verified clean. What
caps it at 7 is one nameable failure class: **contracts that are stated but not
enforced, and seams between excellent subsystems that nobody owns.** Nearly every
serious finding is the code violating one of its own rules.

### Per-domain sub-scores

| Domain | Score | One-line |
| --- | --- | --- |
| ECS + reflection codegen | 7.5 | Core ECS airtight + well tested; reflection-layer enforcement gaps + serializer data-loss paths drag it down |
| Vulkan / NVRHI platform | 7.0 | Sync architecture verified correct; robustness holes (OUT_OF_DATE, format fallback, device loss) are real user-facing failures |
| Mesh / material pipeline | 7.0 | Layout/parity discipline + F2a reservation scheme are senior-level; streaming-era bindless hazard + epoch bug + importer holes |
| Lighting + runtime | 7.0 | Clustered pipeline correct where hardest; hierarchy seams (lights, reparent) + cull occupancy waste |
| Sandbox editor | 7.0 | Undo architecture genuinely impressive; four majors are all integration gaps between good subsystems |
| Core asset / GUID / jobs | 6.5 | Escape-protection + reconcile design right; reachable UB from one corrupt file + two JobSystem livelock traps |
| Build / tests / hygiene | 6.5 | Better-than-industry shader pipeline + warning rigor; self-contradicting dep pins, FP-semantics split, deleted sanitizers |

---

## Falsification pass (2026-07-17)

Each critical was tested by writing a red test (or running an instrumented app)
*before* any fix, to confirm the bug is real rather than theorized.

- **C1 — confirmed** (SIGABRT). Correction to the original finding: the *big
  positive* slot is a lazy ~68 GB reservation that does **not** fault on this
  toolchain; the real crash is the **negative → `resize(0)` → OOB write**. Both
  cases now tested in `TestAssetDatabase.cpp`.
- **C2 — confirmed.** 4 throw sites (`type`/`guid`/`material`/AssetIdJson) raise
  `json.exception.type_error.302`. Tests in `TestAssetDatabase.cpp` + `TestAssetIdJson.cpp`.
- **C4 — confirmed** (mechanism). `TestEditHistory.cpp`: the rebind hook fires for
  a component that sorts before Transform while Transform is still absent.
- **C6 — confirmed** (SIGABRT). `TestMeshImporter.cpp`: a POSITION-less `_LOD0`
  primitive + drawable `_LOD1` → `++Lods[1]` on a size-1 vector.
- **C7 — confirmed.** `TestSceneSerializer.cpp` (file round-trip): one NaN empties
  the whole scene (`LoadFromFile` → false).
- **C5 — confirmed by inspection** (no play-state gate on Save); not unit-tested.
- **C3 — NOT reproduced; downgraded to a false positive.** Instrumented run with
  validation active (confirmed) and the car level streaming 33 materials: the
  bindless `writeDescriptorTable` calls **were** confirmed to run mid-render
  (frames 120–840, prior frames in flight), yet the Khronos validation layer
  raised **zero** VUID-03047 across ~40 mid-flight writes (including a slot
  re-write). NVRHI's `ePartiallyBound` + the append-only `_nextBindlessSlot++`
  (a fresh slot the in-flight frame never dynamically indexed) is spec-legal.
  C3 would only bite if an **in-use** slot were rewritten while pending, which the
  current code never does. Leaving the bindless streaming path as-is.

## Falsification + fix pass — Majors (2026-07-17)

Wrote a red test per falsifiable Major before touching source; each below was
**REPRODUCED** against unfixed code. Four are now fixed (red→green); two are kept as
live `doctest::should_fail()` reproductions because their fix is an open design
decision; the rest have no clean unit seam and are deferred with the app.

- **M2 — fixed (position).** `LightingSystem` now reads `worldMatrix[3]` for point/spot
  light position. Spot *direction* left as-is (whether it should rotate with the parent
  is a separate decision — see the deferred list). No unit seam (device-dependent), so
  inspection-verified.
- **M3 — fixed (REPRODUCED → green).** `Parent` is now `ACOMP(tracked)` and
  `PropagateTransforms` ORs `Changed<Parent>` into the dirty test. Tests:
  `TestHierarchy.cpp` (attach-after-propagation, reparent-to-new-parent).
  Residual: detach via `Remove<Parent>` doesn't stamp — no such site today; noted.
- **M4 — split.** (b) duplicate-name **fixed**: `EnsureFinalized` drops later duplicates
  and logs an error (`TestComponentRegistry.cpp` dup-name test → green). (a) late-Register
  **renumber DEFERRED** (`should_fail` test retained): fix is a design choice
  (assert `!_finalized` / explicit `Freeze()` / decouple id from sort order).
- **M7 — fixed (REPRODUCED → green).** `MeshImporter::AppendPrimitive` swaps triangle
  winding and negates tangent `w` when `determinant(mat3(model)) < 0`.
  Test: `TestMeshImporter.cpp` mirrored-node winding.
- **M11 — DEFERRED (`should_fail` test retained).** Raw-entity context resolves a dead
  slot to its new occupant (generation dropped). Fix is a format/codegen decision
  (carry generation through the raw context, or reject non-`IsAlive` refs).
- **M1, M5, M6, M8, M9, M10, M12 — deferred**, no clean unit seam (device / job-threads /
  editor / build-config). M12 note: C7's non-finite sanitize and `LightingSystem`'s
  `SafeDirection` assume finite-math is NOT optimized away, so the `-ffast-math`
  Release-only split (M12) can turn those guards into dead code in ship — fix together.

## Critical (crash / UB / data loss from realistic input)

- [x] **C1 — Corrupt `.aast` sidecar → OOB write / 64 GiB alloc.**
  `modules/Core/src/AssetDatabase.cpp:181` — `slots.resize(entry.slot + 1)` in
  `uint32_t` arithmetic. A sidecar `"slot": -1` wraps (nlohmann unchecked unsigned
  conversion) to `0xFFFFFFFF` → `resize(0)` then `slots[0xFFFFFFFF]` OOB write;
  `0xFFFFFFFE` → ~64 GiB `bad_alloc` out of `Rebuild()`, uncaught. Contract says
  "skip malformed sidecars, never crash." Fix: validate/clamp `slot` (reject above
  a sane cap) at deserialize, size with `size_t` after validation.
- [x] **C2 — Wrong-typed sidecar/AssetId JSON fields throw through `Rebuild()`.**
  `modules/Core/src/AssetSidecar.cpp:67,72,92,98`, `AssetIdJson.cpp:43` —
  `document.value("type", std::string{})` throws `type_error` when the key exists
  with the wrong type, despite the `allow_exceptions=false` parse. One
  `{"type":123}` sidecar crashes the editor at boot. Fix: `is_string()`/
  `is_number_unsigned()` guards, or wrap in try/catch → `ParseFailed`.
- [~] **C3 (NOT A BUG — see Falsification pass) — Bindless descriptor writes race in-flight frames (Vulkan VUID 03047).**
  `modules/Render/src/AssetCache.cpp:272-280` — streamed texture publishes call
  `writeDescriptorTable` (→ immediate `vkUpdateDescriptorSets`) on a set frame
  N−1's still-pending command buffer bound. Layout is `ePartiallyBound` only, no
  UPDATE_AFTER_BIND (confirmed in vendored NVRHI + `VulkanContext.cpp:348-354`).
  `Clear()` already handles this class with `waitForIdle`; the steady-state path
  re-introduces it. Fix: defer `writeDescriptorTable` into a queue applied after
  the frames that bound the table retire (fence-gated), or enable update-after-bind.
- [x] **C4 — Undo of entity-delete silently loses the Jolt body.**
  `apps/sandbox/src/SandboxApp.cpp:732-755`, `EditHistory.cpp` phase-2 restore.
  Components restore in the registry's *alphabetical* order
  (`RigidBodyDescriptor` < `Transform`), rebind hook fires per-component: the
  descriptor branch needs a Transform that isn't there yet (skips `AddPhysicsBody`),
  and the `kTransform` branch only moves an *existing* body. Revived colliders
  don't simulate until the next Stop/level-load. Fix: defer rebind to a per-entity
  second pass, or make the `kTransform/present` branch create the body when
  `desc && !body` (order-independent).
- [x] **C5 — Save / Save As live during Play → clobbers the level with sim state.**
  `apps/sandbox/src/SandboxLevels.cpp:67-84` — no play-state gate. Saving while
  Playing/Paused writes settled-physics poses over the `.alvl`, then sets
  `_savedStateToken` to the *editing* history token so dirty-tracking reports clean
  after Stop. Fix: gate Save/Save As on `_playState == Editing` (BeginDisabled).
- [x] **C6 — Importer LOD-table OOB write on a degenerate LOD.**
  `modules/Geometry/src/MeshImporter.cpp:577-596` — a dense LOD level whose buckets
  are all non-drawable desyncs `Lods.size()` from the `lod` index;
  `++merged.Lods[lod].SubMeshCount` writes one past the end. Fix: pre-size `Lods`
  to `distinctLods.size()`, index by `lod`, prune trailing empties (or `NoGeometry`
  on empty LOD0).
- [x] **C7 — A single NaN/Inf float bricks the whole saved scene.**
  reflectgen float/vec/quat/mat templates + `SceneSerializer::LoadFromFile:253-267`
  — nlohmann writes non-finite as `null`; Save succeeds, Load throws →
  `scene.Clear()` → entire level loads empty. One physics blow-up + autosave =
  unrecoverable level. Fix: sanitize non-finite on save (→ default), and/or
  per-component try/catch on load so one bad component doesn't void the scene.

## Major

- [x] **M1 — Async publish erases the loading marker before the epoch check.** (fixed; reproduced live via widened race window + fingerprint logs, then verified clean)
  `AssetCache.cpp:208-210` (mesh) + `:439-441` (material) — a stale worker's publish
  deletes the *new* job's `_meshLoading` marker, then drops on the epoch check.
  `HasPendingLoads()` lies (placeholders stick), and a later resolve kicks a
  duplicate import → double `MeshBuffer::Upload` on the same entry (arena range
  leak + wasted id). Level reloads hit this. Fix: erase only when
  `epoch == _loadEpoch`, or key the loading sets by (path, epoch).
- [x] **M2 — Parented lights render at the wrong position.** (position fixed; spot-direction deferred)
  `modules/Runtime/src/LightingSystem.cpp:52,62` reads local `transform.position`;
  every other consumer on the branch reads `worldMatrix[3]`. Fix:
  `glm::vec3(transform.worldMatrix[3])` (+ decide spot-direction parent-rotation).
- [x] **M3 — Reparenting never dirties the child.**
  `Hierarchy.cpp:75,79` only checks `Changed<Transform>`; `Parent` is untracked, so
  attach/detach after the first propagation leaves a stale `worldMatrix` until
  something else moves. Fix: make `Parent` tracked and OR `Changed<Parent>` into
  `localChanged`, or stamp the child Transform at every reparent site.
- [~] **M4 — ComponentRegistry re-finalization renumbers ids + dangles pointers.** (dup-name rejection fixed; renumber DEFERRED)
  `ComponentRegistry.cpp:16-45` — any `Register` after the first query re-sorts and
  renumbers while `ComponentIdOf<T>` caches ids forever → type-confused pool casts.
  Also duplicate component names are accepted and silently overwrite in saves. Fix:
  `ASSISI_ASSERT(!_finalized)` in `Register` (or an explicit `Freeze()`), and
  hard-assert unique names in `EnsureFinalized`.
- [ ] **M5 — Swapchain never recreated on same-size `OUT_OF_DATE`.**
  `VulkanContext.cpp:858-863,920-927` — recovery lives only in the resize callback,
  so a display-mode change / monitor hot-plug / compositor restart freezes rendering
  forever. Fix: `_swapchainStale` flag on OUT_OF_DATE from acquire or present;
  rebuild at the top of the next `BeginFrame` at current framebuffer size.
- [ ] **M6 — Surface-format fallback can pick an unmappable format (boot abort) or
  double-gamma.** `VulkanContext.cpp:576-584` + `ToNvrhiFormat:384-392` — `formats[0]`
  fallback lands in `Format::UNKNOWN` → app refuses to launch even when a mappable
  BGRA8 sits at `formats[1]`; an SRGB `formats[0]` silently double-encodes. Fix:
  scan for the first NVRHI-mappable format matching the gamma convention; log it.
- [x] **M7 — Mirrored (negative-determinant) glTF nodes render inside-out.**
  `MeshImporter.cpp:336-408` bakes node transforms without a winding fix; with fixed
  back-face cull + CCW-front the mirrored geometry culls its visible faces and lights
  wrong. Fix: when `determinant(mat3(model)) < 0`, swap two indices per triangle and
  negate baked tangent `w`. (Adjacent to the car's flipped-normal symptom.)
- [ ] **M8 — JobSystem API traps.** `JobSystem.hpp:302-311` — a second `.Then()`
  silently discards the first continuation (orphaned task → `Wait()` livelocks);
  `Wait()` (`:219-228`) on a chain ending in `Pool::Main` from the main thread
  livelocks (HelpUntil only runs worker tasks). Fix: assert single continuation;
  drain main tasks in `HelpUntil` when on the main thread, or assert/doc loudly.
- [ ] **M9 — Editor thumbnail pipeline.** `SandboxAssetBrowser.cpp:399-427` —
  synchronous full-res decode+upload on the UI thread, unbounded VRAM cache (never
  cleared), no clipper so all tiles resolve per frame → >256 images blows the
  `kMaxDebugTextures` badge pool. Fix: decode/downscale on the job system with a
  placeholder; clipper/visibility gate; LRU cap or clear on directory change.
- [x] **M10 — Gizmo drag mis-commits when Transform header / Inspector collapsed.** (fixed; manually reproduced + verified — no unit seam)
  `SandboxGizmo.cpp:132-135` + `EditHistory.cpp:253-265` — the gesture's liveness is
  owned by the inspector's per-frame `RecordBefore` inside a `CollapsingHeader`;
  collapse it and the sweep commits mid-drag, losing the rest of the gesture. Fix:
  gizmo calls `RecordBefore` every drag frame (idempotent), or keep gestures open
  while `editingActive`.
- [~] **M11 — Raw-entity undo context round-trips `EntityRef` by bare slot index.** (DEFERRED — design decision; `should_fail` test retained)
  `SceneSerializer.cpp:63-91` + EntityRef codegen — under `ScopedRawEntityContext`
  a dead handle serializes its index and resolves to the slot's current occupant;
  after slot reuse an undo snapshot retargets the ref to an unrelated entity. Fix:
  carry generation through the raw context, or reject refs failing `IsAlive`.
- [ ] **M12 — Build system self-contradictions.**
  - `CMakeLists.txt:84-93` — `-mavx2/-mf16c/-mfma/-mlzcnt/-mbmi` unguarded by arch
    (aarch64 won't configure; pre-Haswell SIGILL, no CPU check). Gate on
    `CMAKE_SYSTEM_PROCESSOR`.
  - `CMakeLists.txt:129-139` — `-ffast-math` on `$<CONFIG:Release>` only → dev
    (RelWithDebInfo) and ship binaries have different FP semantics; NaN guards can be
    dead code in ship. Apply to dev too or add `-fno-finite-math-only`.
  - `CMakeLists.txt:382-434` — ImPlot/ImGuizmo/NVRHI use `GIT_SHALLOW` (ImPlot also
    `DOWNLOAD_ONLY`) on bare-commit pins, directly contradicting the stb/imgui
    comments explaining why that breaks fresh clones. Drop those keywords.
  - Sanitizer presets deleted in the same branch that added the JobSystem + async
    streaming they exist to check. Restore `*-asan`/`*-tsan` presets.

## Minor (selected)

- Uninitialized GPU mip level on failed downsample → samples garbage at distance
  (`Texture.cpp:77-84,134-143`).
- MSAA resolve + alpha blending in gamma space → dark edge fringing
  (`PostProcess.cpp:236,242`, `cube_min.frag:345`); render scene into an SBGRA8
  offscreen target and drop the shader `pow`.
- `cluster_cull.comp:9` dispatches 3456 single-thread workgroups (~3% occupancy) and
  re-transforms every light per cluster per pass; transform lights to view space once
  and use `local_size_x=64`.
- Material-table overflow past `kMaxMaterials` (4096) still mints ids that index out
  of the table → bindless UB (`AssetCache.cpp:319-328`); clamp to fallback row 0.
- `msaaSamples` from user-editable JSON never validated vs device sample-count caps
  (`PostProcess.cpp:80-96`).
- Shader NaN sources with one-line fixes: zero-length spot cone
  (`smoothstep` 0/0), fragment at light position (`toLight/0`), degenerate default
  tangent `(1,0,0,1)` ∥ ±X normal (`cube_min.frag:155,309,333`); + `uint(negative)`
  UB in `ClusterIndex()` (`:238`).
- `SceneSerializer::LoadFromFile:258` catches only `nlohmann::json::exception`; a
  hook throwing anything else leaks a half-populated scene. Catch `std::exception`.
- Stale eyedropper/asset-browser targets alias entities across level load
  (`Scene::Clear` resets generations densely) — clear armed targets in
  `LoadLevelFromPath`.
- `StopPlay` skips restore when the snapshot is empty → play-created entities leak
  into Editing (`SandboxPlay.cpp:147`).
- Removing Transform leaves an orphaned live Jolt body (`SandboxInspector.cpp:653`).
- `resizeDescriptorTable` is a no-op on Vulkan (assert-only) — the grow/shrink
  bookkeeping in AssetCache is dead and `writeDescriptorTable`'s `false` return
  (capacity 16384 exceeded) is unchecked.
- Per-frame heap allocs in both draw paths (`Renderer.cpp:101` CPU items;
  `MeshPass.cpp:258-262` instances/commands/batchMeshes) — reuse as members.
- ~139 explicit-width-int violations incl. a public header
  (`GpuTelemetry.hpp:19-30` `unsigned long long`); reflectgen's type table
  institutionalizes bare `int` while rejecting `int64_t` etc. (backwards).
- Duplicate-GUID collision never self-heals (losing file stays id-less forever) and
  is untested (`AssetDatabase.cpp:200-210`); directory-walk `ec` warning is dead code
  (`:115-123`); `MintAssetId` static RNG not thread-safe (`:58-90`).
- Sidecars minted into the **build-tree** asset copy: `DiscoverRoot` walks up from the
  exe dir first, so a normally-launched editor writes `.aast`/`.amat` into the
  ephemeral build dir, not source — a GUID-stability hazard for fresh clones
  (`AssetSystem.cpp:417-442` + `apps/sandbox/CMakeLists.txt:38-53`).
- Asset staging `copy_directory` never prunes → deleted source assets (incl. stale
  `.spv`) linger in the build tree (`apps/sandbox/CMakeLists.txt:38-47`).
- `profiles/` is dead-but-maintained Conan config; setup scripts append to tracked
  root CMakeLists + emit a dead `ASSISI_APP` var.

## Cross-cutting themes (the diagnosis)

1. **Malformed input is the weakest axis.** Codegen is default-deny/fail-loudly and
   executes it beautifully; the *runtime* input boundaries (sidecars, saved scenes,
   options JSON, glTF edge cases) get trusting code with UB behind it.
2. **Subsystems strong; seams unowned.** Lights vs hierarchy, undo vs registry
   ordering, gizmo vs inspector, streaming vs in-flight frames, asset-root discovery
   vs the build copy — each side correct in isolation.
3. **Stated contracts lack teeth.** "immutable after startup", "at most one
   continuation", "explicit-width ints", "no GIT_SHALLOW on bare commits" — all
   written down, none asserted, all violated somewhere. A dozen `ASSISI_ASSERT`s
   convert most majors into loud startup failures.
4. **Safety tooling regressed exactly when it was needed** (sanitizers deleted
   alongside the new threading/async).

## What's genuinely excellent (protect this)

- Every GPU struct has a C++ mirror + `static_assert` + accurate binding-offset
  cross-references in both languages — zero layout mismatches found.
- Frames-in-flight design (per-slot acquire semaphores, per-image render-finished,
  event-query throttle) is the correct modern pattern, documented per-semaphore.
- CPU cull path retained as a pixel-exact oracle for the GPU path.
- Undo system (delta transactions over reflected JSON, `ReviveAt` identity revival,
  play-session survival) with 20 focused tests.
- Commit hygiene at 255 commits: staged, single-topic; dead code deleted not
  commented; debt tracked with rationale, zero TODO litter.

## Path to 8+

None of the criticals need redesign — the fix list is asserts, input validation, a
deferred-descriptor-write queue, a rebind-ordering fix, a Save play-state gate, and
build-flag hygiene (days, not weeks). Restore sanitizer presets; add tests for the
seam scenarios (reparent-after-propagation, corrupt sidecar, undo-delete-with-physics,
duplicate component names, mirrored-transform import). That hardening pass is the
difference between 7 and a defensible 8.5.
