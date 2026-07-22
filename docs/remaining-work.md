# Remaining work — the non-deferred backlog

Captured 2026-07-21. This is the inverse of the deferred lists scattered across
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
  loopback echo test in `modules/Net/tests` passing in CI.
- **Stage 2 — headless Application.** Split `Initialize()` into
  `InitializeCore()` / `InitializePresentation()`; `AppConfig::headless` +
  `--server`; loop paced by `SleepUntil` against `_closeRequested` instead of
  window/vsync; `OnRender` gets a default no-op body. Independently valuable
  (headless CI sim tests) and independent of Stage 0 — either can land first.
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

## 2. Code-review round 6 — unfixed, undeferred remainder

C1–C7 and most majors are fixed (`code-review-2026-07-round6.md` has the
disposition per item). Still open with no deferral rationale:

- **M8 — JobSystem API traps** (`modules/Core/include/Assisi/Core/JobSystem.hpp`).
  Verified still present: `HelpUntil` runs *worker* tasks only, so `Wait()` on a
  chain ending in `Pool::Main`, called from the main thread, livelocks; and a
  second `.Then()` on the same task silently discards the first continuation
  (orphaned task → a later `Wait()` livelocks). Fix: drain main-queue tasks in
  `HelpUntil` when on the main thread; assert single continuation. Small, and
  directly under the networking work — do it before more async lands on top.
- **M12 remainder — build-system hygiene** (partially fixed; `GIT_SHALLOW` on
  bare-commit pins was resolved in `63a292a`). Verified still open:
  - **Sanitizer presets deleted.** The `ASSISI_ENABLE_SANITIZERS` machinery
    survives in `CMakeLists.txt` but no `*-asan`/`*-tsan` preset exists in
    `CMakePresets.json` to turn it on. Restore them — they were deleted in the
    same branch that added the JobSystem + async streaming they exist to check,
    and networking adds another thread (GNS's service thread) on top.
  - **`-ffast-math` on Release only** (`CMakeLists.txt` perf-knobs block): dev
    (RelWithDebInfo) and ship binaries have different FP semantics, so the C7
    non-finite save guards and `LightingSystem::SafeDirection` can be dead code
    exactly in ship. Apply consistently or add `-fno-finite-math-only`. Note
    the networking design *assumes* fast-math stays (it's a reason lockstep was
    rejected) — resolve the split, don't silently drop the flag.
  - **`-mavx2/-mf16c/-mfma/-mlzcnt/-mbmi` unguarded by architecture**
    (Assisi-Options block): aarch64 won't configure; pre-Haswell x86 gets
    SIGILL with no CPU check. Gate on `CMAKE_SYSTEM_PROCESSOR`.
- **Round-6 minors — undispositioned backlog.** The "Minor (selected)" list in
  the round-6 doc was never triaged item-by-item (some were fixed in passing,
  e.g. the thumbnail pipeline; most were not). Worth one pass to sort into
  fix-now / defer-with-reason. Highest-value candidates: material-table
  overflow past `kMaxMaterials` → bindless UB; uninitialized GPU mip on failed
  downsample; `resizeDescriptorTable` no-op + unchecked `writeDescriptorTable`
  return; sidecars minted into the build-tree asset copy (GUID-stability
  hazard); per-frame heap allocs in both draw paths.

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

## 5. Decisions waiting on the user (unblock further work; zero code until decided)

Not deferred *work* — deferred *choices*. Each blocks or shapes an item above:

- **Template Phase 3:** preserve the editor tooling as a separate `apps/editor`
  (the plan's recommendation) or shelve it in git history. Gates Phase 2.
- **M4a:** `ComponentRegistry` late-`Register` renumbering — assert
  `!_finalized`, explicit `Freeze()`, or decouple id from sort order. A live
  `should_fail` test documents the bug.
- **M11:** raw-entity undo context resolves dead slots to their new occupant —
  carry generation through the raw context, or reject non-`IsAlive` refs.
  Also a live `should_fail` test.
- **M2 follow-on:** should a parented spot light's *direction* rotate with its
  parent? (Position was fixed; direction was left as-is pending this call.)
- **M6:** surface-format fallback can pick an unmappable or double-gamma
  format. Device-dependent, previously left to the user; fix is a
  scan-for-first-mappable-format loop if/when wanted.
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
