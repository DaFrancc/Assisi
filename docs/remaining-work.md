# Remaining work — the non-deferred backlog

Captured 2026-07-21; **updated 2026-07-22** — §2 fully cleared (branch
`hygiene/round6`: the JobSystem API traps, the build-system hygiene items, and
the whole round-6 minors list), and §4b/§4c added for gaps that turned out to be
undocumented. This is the inverse of the deferred lists scattered across
the other docs: everything still open that is **not** consciously parked. If an
item is here, it is actionable now — nothing below is waiting on scale,
first-ship, or a future subsystem. Verified against source at capture time, not
just against the (sometimes stale) status headers of the other docs.

The last section lists what is *excluded* and why, so this doc can't be misread
as "everything else is done."

## 1. Networking — stages 0–6 (`networking-design-notes.md`)

The declared next milestone. Design settled 2026-07-20; **nothing built**
(no `modules/Net`/`NetSync`, no headless split in `Application`, no `simTick`).
Stage 7 (prediction, lag compensation, interest management, NAT-traversal
production infra) is explicitly deferred and excluded here.

- **Stage 0 — GNS build integration.** protobuf(+abseil) + crypto backend + GNS
  via FetchContent, static, warnings-clean, both platforms. The designated risk
  stage; timeboxed, with a four-step escalation ladder in the doc. Done =
  loopback echo test in `modules/Net/tests` passing locally on both compilers
  (there is no CI — see §4c).
- **Stage 2 — headless Application.** Split `Initialize()` into
  `InitializeCore()` / `InitializePresentation()`; `AppConfig::headless` +
  `--server`; loop paced by `SleepUntil` against `_closeRequested` instead of
  window/vsync; `OnRender` gets a default no-op body. Independently valuable
  (headless sim tests, runnable locally) and independent of Stage 0 — either can
  land first.
- **Stage 1 — `Assisi::Net` transport wrapper.** Pimpl over GNS, dense
  `ConnectionId`, lanes, `Poll()` on the main thread, `CreateLoopbackPair` for
  the listen server. Needs Stage 0.
- **Stage 3 — sim tick + input commands.** `simTick` in the fixed-step loop,
  `InputCommand` sampled per tick from `ActionMap`, per-connection command
  queues. Forces player-controlling systems off direct `IsKeyDown` polling.
- **Stage 4 — binary codec over FieldMeta.** `ByteWriter`/`ByteReader`,
  `Write/ReadComponent` walking reflection metadata, protocol hash at handshake.
  Unit-testable in isolation.
- **Stage 5 — replication core.** `ReplicationServer`/`ReplicationClient`,
  NetId map, delta replication off change ticks, snapshot interpolation.
  **Blocker to resolve first, in ECS:** `Query` yields mutable references
  without stamping change ticks, so mutations through queries produce no delta.
  Fix (stamping query variant vs. enforced `GetMut` discipline) must land
  before this stage — the doc leans toward the stamping variant.
- **Stage 6 — listen server + sandbox Host/Join UI.**

Suggested order (from the 2026-07-21 review of the docs): Stage 2 → Stage 0 →
1 → 3/4 → 5 → 6, with §2 below as a short preface.

## 2. Code-review round 6 — cleared

C1–C7 and every major are fixed (`code-review-2026-07-round6.md` has the
disposition per item). **This section is now clear** — M8, M12, and the whole
minors list were resolved on 2026-07-22 (branch `hygiene/round6`), and the four
round-6 items that had been waiting on a decision are resolved in §5.

- ~~**M8 — JobSystem API traps**~~ **Fixed 2026-07-22** (`7ded872`, branch
  `hygiene/round6`). `HelpUntil` gained a `helpMain` flag (passed by
  `Task::Wait`) that also drains main-queue tasks when the caller is the
  thread that constructed the JobSystem, so a chain ending in `Pool::Main` no
  longer livelocks; `ParallelFor` keeps worker-only helping so main tasks still
  run only at the `DrainMain` safe point. A second `.Then()` now fires
  `ASSISI_ASSERT` (`TaskState::continuationClaimed`) instead of orphaning the
  first continuation. Four regression tests added, all bounded so a
  reintroduced bug fails rather than hangs. Clean under TSan.
- ~~**M12 remainder — build-system hygiene**~~ **Fixed 2026-07-22** (`GIT_SHALLOW`
  was already resolved in `63a292a`; the rest on `hygiene/round6`):
  - **Sanitizer presets** restored in `37aac31`: `gcc-asan`, `gcc-tsan`,
    `clang-asan`, `clang-tsan`, `msvc-asan` (MSVC supports ASan only), each
    inheriting its `-debug` preset, with matching build/test presets and
    `make gcc-asan` / `make test-gcc-tsan` targets. Deps don't link
    `Assisi::Sanitize`, so third-party code stays uninstrumented.
  - **`-ffast-math` split** resolved in `5da431a`: now applies to Release *and*
    RelWithDebInfo, with `-fno-finite-math-only` appended so the non-finite
    guards stay live (verified empirically — under plain `-ffast-math` an
    `isfinite` guard accepts both NaN and Inf). Debug stays strict FP. The flag
    stays on, as the networking design assumes.
  - **SIMD flags gated** in `5da431a` on an x86 `CMAKE_SYSTEM_PROCESSOR` match,
    so aarch64 can configure; the Haswell x86 baseline is now explicit and
    intentional (pre-Haswell x86 still gets SIGILL by design — a runtime CPU
    check was not added).
  - **New, found by the restored presets** (`b738452`): UBSan's `vptr` check
    could not link, because Jolt builds itself `-fno-rtti` and vptr needs
    typeinfo for every polymorphic type. `-fno-sanitize=vptr` is now applied
    whenever the sanitizer set includes `undefined`. Exactly the kind of latent
    breakage the presets exist to surface.
- **Round-6 minors — undispositioned backlog.** The "Minor (selected)" list in
  ~~the round-6 doc was never triaged item-by-item.~~ **Triaged and largely
  cleared 2026-07-22** on branch `hygiene/round6`. Dispositions below.

  **Fixed:**
  - Material-table overflow → bindless UB, and the unchecked
    `writeDescriptorTable` return (`984e361`). Ids are bindless indices the GPU
    cannot bounds-check, so `MintMaterialId` saturates onto the fallback row and
    MeshPass asserts the invariant in debug. The `resizeDescriptorTable`
    grow/shrink bookkeeping was dead (Vulkan implements it as an assert-only
    no-op) and is deleted rather than left looking load-bearing.
  - Uninitialized GPU mip on failed downsample (`984e361`) — the chain is now
    truncated instead of leaving an unwritten level for the sampler to read.
  - All four shader NaN/UB sources (`984e361`): degenerate tangent, fragment at
    a light's position (point and spot), `inner == outer` cone, and
    `uint(negative)` in `ClusterIndex`.
  - `SceneSerializer::LoadFromFile` catching only `json::exception`, stale
    eyedropper/asset-browser targets across level load, `StopPlay` leaking
    play-created entities when the snapshot was empty, and removing a Transform
    orphaning a live Jolt body (`080a6e4`).
  - Sidecars minted into the build-tree copy (`da038dd`) — see §2's M12 notes;
    also the duplicate-GUID collision that never self-healed, the unreachable
    directory-walk error check, and `MintAssetId`'s unsynchronized RNG.
  - Stale staged assets never pruned, and dead `profiles/` + `ASSISI_APP`
    (`6853bb3`).
  - Explicit-width ints in the public `GpuTelemetry.hpp`, plus reflectgen's
    backwards type table — bare `int` is now rejected by the default-deny with a
    message naming the fix, and `int64_t`/`uint64_t` are supported
    (`370917a`). Per-frame draw-path allocations in `MeshPass::Submit` reused as
    members in the same commit.
  - `msaaSamples` never validated against device caps (`334cdec`).

  **Deferred, with reason:**
  - **MSAA resolve + alpha blending in gamma space.** Superseded by lighting
    stage L2, which replaces the very lines the fix would touch: the scene
    target becomes RGBA16F (not the SBGRA8 the review suggested) and gamma
    encoding moves out of `cube_min.frag` into a dedicated tonemap pass. Fixing
    it now would be rewritten by L2. Fold it into L2 instead.
  - **`cluster_cull.comp` dispatch shape** (3456 single-thread workgroups, ~3%
    occupancy; lights re-transformed per cluster). A real optimization and
    genuinely orthogonal to both design docs, but it is a perf rewrite touching
    the atomic light-index reservation that the review singled out as correct,
    and nothing has profiled it as a bottleneck. Wants a measurement first —
    naturally paired with the lighting or light-culling work.
  - ~~**The wide explicit-width sweep.**~~ **Done 2026-07-22** (`4e144a7`) —
    deferred for conflict risk, then done immediately since no feature branch
    was in flight to conflict with. Files containing a bare integer type: 75 →
    40. The 40 that remain are contracts rather than choices, and should stay:
    NVML's ABI mirror (`GpuTelemetry.cpp`, zero diff), stb_image/libwebp buffer
    types, GLFW callback signatures and write-through locals, Jolt overrides,
    ImGui `int*` out-parameters, `int main`/`%.*s`, Win32/POSIX interop, and
    `char`-as-text. Verified behaviour-neutral: `-Wsign-conversion` unchanged at
    28, all categories matching, ASan + TSan clean.
  - **`DrawScene`'s per-frame `std::vector<DrawItem>`** (`Runtime/Renderer.cpp`).
    Unlike `MeshPass::Submit` this is a free function with no object to hang
    scratch off, so reuse means threading a caller-owned buffer through
    `DrawSceneParams` and its call sites. It is also the CPU fallback path, not
    the GPU-driven one. Worth doing with the F2b/G work that touches the same
    extract loop.

## 3. Engine-as-template conversion (`template-conversion-plan.md`)

Older thread (branch `cleanup/jul-08-26`), never finished, never marked
deferred:

- **Phase 1 visual verify** is still flagged pending in the doc: run the
  sandbox and confirm the `SceneRenderer` extraction changed nothing (lit
  cubes, fly camera, F12 MSAA/FXAA toggle, resize without froxel artifacts).
  Likely long since true in practice — verify and clear the flag, or fold this
  into the next graphics session.
- **Phase 2 — reshape `apps/sandbox` into the actual template**: single scene
  (cubes + ground + light), fly camera, physics, one small example gameplay
  system, collapse the two-scene setup. Blocked in practice on the Phase 3
  decision (you can't strip the sandbox down before deciding where the editor
  tooling goes).

## 4. Lighting — stages L1–L5 (`lighting-design-notes.md`)

Decided 2026-07-21; v2 2026-07-22 (evidence sweep, 12 verification checks);
**v3 2026-07-22** (two adversarial design reviews + blind adjudication —
budgets, mobility-flag prerequisite, transparency/volumetrics scoping, leak
gates, and honest cost models added); **nothing built.** Goal: modern look,
pay-for-what-you-place cost, no ray-tracing requirement, point shadows
first-class, correctness by default. Full staging, budgets, and definitions
of done are in the doc; new prerequisites: mobility flag, GPU timestamp
queries, HDR internal asset serialization.

- **L1 — directional cascaded shadow maps** (stable/snapped, PCF; supersedes
  the shadows entry in the excluded list below).
- **L2 — HDR pipeline:** RGBA16F scene target, filmic tonemap replacing
  Reinhard, bloom, manual exposure. Biggest look-per-effort item in the plan.
- **L3 — local-light shadow atlas** (spot + point, cached static/dynamic
  split, importance-based tile budgeting, per-light `castsShadows` flag —
  the DOOM/Godot/Flax pattern; point shadows are no longer deferred).
- **L4 — DDGI-style baked probe grid** (octahedral irradiance + depth,
  Chebyshev visibility — not plain SH9, which provably leaks); replaces the
  `kAmbient` constant; baked with the engine's own forward pass.
- **L5 — reflection probes / prefiltered specular IBL**, cluster-indexed.
- **L6 — runtime probe refresh**, opt-in, N-probes-per-frame budget (N=0 =
  baked mode, the default).

Excluded by design (evidence in the doc's rejected-techniques section): L7
ray backend (don't build speculatively), explicit ray tracing, SDFGI/VXGI/
radiance cascades/screen-space GI, virtual shadow maps, moment/variance
shadow formats, lightmaps.

## 4b. Transparency / alpha modes — new, and a prerequisite of L3

Added 2026-07-22. Previously parked as "out of scope" in
`mesh-material-architecture.md` §2, which hid a dependency: **lighting L3
specifies alpha-tested shadow casters, and the material system has no
`alphaMode`/`alphaCutoff` for the depth-only alpha-test variant to read.**
`lighting-design-notes.md`'s transparency section likewise assumes a transparent
forward pass. Neither doc referenced the other; both now do.

- **(a) Data + masked bucket — do before L3.** `alphaMode`/`alphaCutoff` through
  the `.amat` schema (per-field deserialization makes it non-breaking), the
  material table, and the sort key's pipeline bits. Yields alpha-test/cutout and
  unblocks L3. Small.
- **(b) Blended transparent pass — the real work.** Separate depth-major sort
  key (already designed, `mesh-material-architecture.md` §2 "the two-key split
  is the transparency seam"), transparent forward pass sampling the same cluster
  lists/atlas/probes. OIT stays out of scope.
- `doubleSided` stays deferred separately — per-material rasterizer state, no
  consumer waiting.
- The importer already warns on both (`MeshImporter`: "has a non-opaque material
  … importing as opaque"), so affected content is discoverable today.

## 4c. Infrastructure gaps (found 2026-07-22; previously undocumented)

These were real and unrecorded — the docs assumed some of them existed.

- **No CI, by choice (2026-07-22).** A GitHub Actions workflow was written and
  then dropped at the user's direction; it is recoverable from the
  `backup/ci-workflow-removed` branch if that ever changes. The consequence to
  keep in mind: **other docs are written as though CI exists** — networking
  stage 0's definition of done is "loopback echo test passing in CI", and
  `ASSISI_WARNINGS_AS_ERRORS` is described as "recommended for CI". Those
  acceptance criteria need rewording against a local command
  (`make gcc-debug && ctest --preset gcc-debug`), or they are unmeetable as
  written. Verification is manual: run the presets, and `scripts/run-sanitized.sh`
  for the sanitizer builds.
- **Windows and macOS are unverified.** MSVC presets exist and have never been
  built. This matters more since the explicit-width sweep: `long` is 32-bit on
  Windows and 64-bit here, so a width mistake would only show up there. With no
  CI, this can only be caught by building MSVC by hand.
- **`-Werror` is off everywhere.** `ASSISI_WARNINGS_AS_ERRORS` exists but the
  tree has warnings today (sign-conversion in the sandbox; reflectgen's
  generated aggregates omit trailing `FieldMeta` members). Until those are
  cleared the option is decoration, and nothing enforces it either way.
- **Tracked levels may reference assets that are not in the repo — accepted
  2026-07-22, do not "fix".** `levels/Materials.alvl` references models under
  gitignored `assets/models/`, so a fresh clone cannot resolve them. This is
  fine by design: an unresolvable reference degrades to the fallback material
  and a warn-once (`AssetCache::_missingMeshWarned`), not a crash or a corrupt
  load. Large binary test assets stay out of git deliberately.
- **Threading coverage is bounded by unit tests that run in 0.26 s.** TSan only
  finds races on paths that execute, and there is no soak or stress test for the
  job system or async asset streaming under load. Networking adds a third thread
  (GNS's service thread) on top of that blind spot. A long-running randomized
  load test would be worth more than another unit case.
- **Uninitialized reads are uncovered.** Neither ASan nor TSan detects them; that
  is MemorySanitizer, which needs the whole dependency tree instrumented.
  Accepted for now — recorded so the coverage boundary is explicit.

## 5. Decisions waiting on the user (unblock further work; zero code until decided)

**Only two remain** — the four round-6 items below were decided and fixed on
2026-07-22 (branch `hygiene/round6`); they are kept with their rationale so the
reasoning is not lost.

Not deferred *work* — deferred *choices*. Each blocks or shapes an item above:

- **Template Phase 3:** preserve the editor tooling as a separate `apps/editor`
  (the plan's recommendation) or shelve it in git history. Gates Phase 2.
- ~~**M4a**~~ **Fixed 2026-07-22** (`4bc09e2`): `Register` now refuses once an id
  has been issued — error naming the component, then an assert. Decoupling id
  from sort order would have traded the bug for non-determinism (the name sort is
  what makes ids reproducible across builds), and appending a fresh id would
  dangle the pointers `ById()`/`All()`/`_serializable` hold into `_metas`. The
  Core tests now register fixtures from a static initializer, as generated
  registrations do.
- ~~**M11**~~ **Fixed 2026-07-22** (`4bc09e2`): the raw-entity context packs
  (slot, generation) and resolves only on an exact generation match, so a ref to
  a recycled slot yields `NullEntity` instead of its new occupant. Rejecting
  non-`IsAlive` refs — the other option on file — would not have worked: a
  recycled slot is perfectly alive. `EntityRef` codegen widened to 64-bit.
- ~~**M2 follow-on**~~ **Decided and fixed 2026-07-22** (`83b68b9`): yes, a
  parented spot light's direction rotates with its parent — `direction` is local
  and is rotated by the propagated world matrix, matching what position already
  did. Consequence worth knowing: an *unparented* light's own rotation now aims
  it too. Covered by `LightingSystem::WorldSpotDirection` tests.
- ~~**M6**~~ **Fixed 2026-07-22** (`83b68b9`): explicit priority scan — ideal
  pair, else any mappable linear format, else a mappable sRGB one with a warning,
  else fail with the reason. An sRGB surface double-encodes gamma against
  `cube_min.frag`'s own `pow`, so it stays a last resort until L2 moves that
  encode into the tonemap pass.
- **Milestone order: networking stages vs. lighting stages.** Both are now
  fully designed with nothing built; §1 and §4 don't depend on each other.
  Pick which thread runs first (or interleave — L2 is small enough to slot in
  anywhere).

## Excluded — deferred by design (do not resurrect without cause)

Listed so their absence above is legible; each has its rationale in its own doc:

- **F2b / F2c / G** (screen-size LOD, dirty-tracked object mirror, HZB
  occlusion) — until instance counts make the CPU extract loop the bottleneck.
  The car/car_lod test assets for F2b are already committed and waiting.
- **Asset DB S5** (cooker + `PakProvider`) — until first ship.
- **Streaming layers 2–4** (loading screen, chunk residency, mip streaming) —
  streaming-era; layer 1 (async load) is built.
- **Light culling ladder** (frustum pre-cull, depth pre-pass, world chunks) —
  per `light-culling-design-notes.md`. (Shadows were listed here until
  2026-07-21; they are now stage L1 of the lighting plan, §4. Lights through
  walls stays expected behavior only until L1 lands.)
- **Frame profiler** — parked until planned properly (Tracy-vs-custom is the
  first question).
- **Job system stages 3–6** (Jolt-pool adapter, coroutine surface,
  work-stealing) — each has a named trigger in `job-system-design-notes.md`.
- **Networking stage 7** (prediction, lag comp, interest management, auth).
- **Undo/redo divergence checker** — the one deferred item of a
  feature-complete system.
