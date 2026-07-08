# Codebase review, round 2 — 2026-07-07

Second full first-party review (~9.9k lines: all of `modules/`, `apps/sandbox`,
`tools/reflectgen`, build glue, tests), done independently and *then* compared
against `code-review-2026-07.md`. Verified that nearly every checked item in
round 1 is actually fixed in the code (generation check in `SparseSet::Has`,
transparent `ActionMap` lookup, `lexically_relative` escape check, GLFW
callback ownership, `Initialize() -> bool`, error-convention enforcement).

Scored **7/10** — up from round 1's 6. The fixes were real and the foundations
are strong. Held at 7 by: two latent data-corruption bugs on the ECS/serializer
spine, an unused abstraction layer already exhibiting copy-paste drift, a
split-brain asset-resolution story, and standards that exist as config files
rather than as gates (no CI).

Checkboxes so this can be burned down like the round-1 doc was.

## Principles decided during this review

- **The score is capped by what is *provisional*, not by what is *missing*.**
  Missing features don't lower the score; promissory notes do — stubs that
  store a lie (`SetVSyncEnabled`), caches documented as "wrong the day X
  exists" (`MeshPass::_bindingSetCache`), contracts documented as "silent UB,
  don't do that" (`Scene::Query`). A small engine with zero placeholders and
  every contract enforced can score a 9. Resolving a promissory note by
  *deleting* the affordance is as valid as fulfilling it.
- **No new promissory notes.** Every new subsystem lands with its contracts
  enforced (asserts, not doc comments) and its risky paths tested.
- **Abstraction quality is empirical.** `SystemRegistry`, the reflection
  layer, and the App API each have exactly one consumer, so "this is
  well-designed" is currently an unverifiable claim. 9+ requires a second
  consumer and real data to verify it. No amount of in-place polish generates
  that evidence.

## Correctness bugs (real, not taste)

- [x] **`SparseSet::Add` with a stale handle corrupts the live occupant's
  mapping.** `SparseSet.hpp:44-59`. *Fixed:* `Add` now rejects any occupied
  index slot (returns nullptr) instead of overwriting the live occupant's
  sparse entry. Per feedback the `std::expected<T*, SparseSetError>` return
  collapsed to a bare `T*` — a duplicate and a stale handle both just mean "not
  added" and the caller can't act on the distinction; the `SparseSetError` enum
  is gone. Two layers: `SparseSet::Add` rejects an occupied-by-other-generation
  slot (defense-in-depth for direct pool users), and `Scene::Add` now gates on
  `IsAlive` — the pool check alone can't catch a stale add when the live entity
  hasn't populated that pool yet (the "`Scene::Add` does not check `IsAlive`"
  gap this item called out). Regression tests: "adding a stale handle over a
  live slot is rejected" (`TestSparseSet.cpp`) and "a stale handle cannot add to
  a reused slot" (`TestScene.cpp`) — both confirmed red before their fix. The round-1 generation fix protected
  `Has`/`Get` but left `Add` as a corruption vector. Failure sequence:
  entity `{index=5, gen=0}` is destroyed; the slot is reused by `{5, gen=1}`,
  which adds a component landing at dense pos 3 (`_entities[3] == {5,1}`,
  `_sparse[5] == 3`). Now `Add({5, gen=0}, ...)` is called with the stale
  handle: `Has()` correctly returns false (generation mismatch), so `Add`
  *proceeds* — and overwrites `_sparse[5]` to point at the new dense slot
  (say 7, `_entities[7] == {5,0}`). Consequences: (a) the live entity `{5,1}`
  loses its component — `Get({5,1})` now finds pos 7, sees gen 0 ≠ 1, returns
  null; (b) dense slot 3 is orphaned but still occupied, so a later
  swap-remove that moves the last element into an orphaned slot writes
  `_sparse[_entities[pos].index]` from stale data and the desync spreads.
  Nothing upstream saves you: `Scene::Add` (`Scene.hpp:59-62`) does not check
  `IsAlive`. Fix: `Add` must detect the index-occupied-by-different-generation
  case and reject (return a new `SparseSetError::StaleHandle` or similar) —
  do NOT silently treat it as fresh, that hides use-after-destroy bugs in
  caller code. **A unit test must land with the fix, same as round 1's
  generation fix.**
- [x] **`SceneSerializer::Load` breaks forward entity references — silent
  hierarchy loss on round-trip.** `SceneSerializer.cpp:142-159`. *Fixed:*
  two-pass load — pass 1 creates every entity and fills `indexToEntity`
  completely, pass 2 deserializes components so any forward `EntityRef`
  resolves. New `Assisi-Runtime-Tests` suite covers it: a natural-order
  round-trip whose child sorts before its parent, plus a hand-authored
  child-before-parent fixture (both confirmed red on the single-pass loader). Load creates
  entities one at a time and deserializes their components immediately, but
  `IndexToEntity` (`SceneSerializer.cpp:59-65`) can only resolve entities
  created *so far* — `s_context->indexToEntity` grows as the loop runs, and
  an out-of-range index returns `NullEntity`. `ParentComponent` serializes
  `ECS::Entity parent` as a serial index (`Hierarchy.hpp:29`, reflectgen's
  EntityRef codegen), so a child whose serialized position precedes its
  parent's loads with `parent == NullEntity` — the hierarchy silently
  flattens. Save order is sorted by `(generation << 32) | index`
  (`SceneSerializer.cpp:77-94`), which does NOT put parents first: any
  slot-reuse during editing (destroy an entity, create the parent in the
  reused slot → parent has higher generation → sorts after its child) produces
  exactly this. Fix: two-pass load — pass 1 creates all entities and fills
  `indexToEntity` completely; pass 2 adds components. **Land with a
  round-trip test whose fixture has a child serialized before its parent.**
- [x] **`PropagateTransforms` has no cycle guard — parent cycle = stack
  overflow.** `Hierarchy.cpp:27-42`. *Fixed:* an `onChain` set tracks entities
  on the current recursion stack; when an entity's parent is already on the
  chain the cycle is broken (logged with the entity index/gen, node treated as
  a root) instead of recursing to death. Covered by "a parent cycle does not
  overflow the stack" in `TestHierarchy.cpp`, alongside root and parent-compose
  cases. Secondary note left open (perf, not correctness): the function
  allocates a fresh `unordered_map` + closures every call and runs twice per
  frame — keep a scratch map as a member/static or pass one in.
- [x] **`~VulkanContext` leaks the instance and surface on partial init.**
  The destructor early-returned when `_device == VK_NULL_HANDLE`, so a
  `Create()` failure after the instance/surface existed leaked both. *Fixed:*
  the device-scoped teardown is guarded on `_device`, and the surface and
  instance are destroyed on their own handles afterward (instance-level dispatch
  is loaded as soon as `_instance` exists, so this is safe on every partial
  path).
- [x] **`HandlePhysicsEditing` keys off `ImGui::IsAnyItemActive()` globally —
  any widget interaction anywhere freezes the selected entity's physics.**
  *Fixed:* the drag check is scoped to the Inspector by conjoining
  `IsAnyItemActive()` with `IsWindowFocused(RootAndChildWindows)`. The handler
  runs inside the Inspector's Begin/End, so `IsWindowFocused` (current window)
  is true only while an Inspector widget is the one being manipulated —
  touching another window's widgets no longer forces the body to `Static`.
  (Runtime ImGui behaviour; verify by interacting with the AA combo while an
  entity with a rigid body is selected.)

## Architecture

- [ ] **`GameApplication` is a dead layer already diverging — delete it or
  port the sandbox onto it.** Zero users: `apps/` contains only `sandbox`,
  and `SandboxApp` derives from `Application` directly, reimplementing what
  `GameApplication` provides. The duplication is already manifest: the
  game.json input-binding block is a verbatim copy
  (`GameApplication.cpp:20-38` vs `main.cpp:174-193`, down to a slightly
  different warning string). Two parallel bring-up paths, one untested and
  unused. Per the provisional-code principle: either the sandbox becomes
  `GameApplication`'s first consumer (which finally exercises it), or it gets
  deleted until a second app exists. Keeping both is the worst option.
- [x] **Asset resolution is two systems; shaders bypass `AssetSystem`
  entirely.** *Fixed:* one filesystem story, `AssetSystem`, now with two
  escape-protected mounts sharing the same virtual-path scheme. (1) A
  **read-only asset root** (unchanged) for shipped content. (2) A **read-write
  user root** for per-user data — `ResolveUser`/`ReadUser*`/`WriteText`/
  `WriteBinary`/`UserExists`, defaulting to the exe dir (CWD-independent),
  overridable via `ASSISI_USER_ROOT` or `SetUserRoot()`. The split is
  deliberate: a shipped game's install dir is often not writable, so runtime
  writes must not target the asset tree — this is the "read-write asset system"
  a real game needs, not a writable asset root. Shaders: `ReadSpirvFile`'s raw
  `std::ifstream` is gone — `LoadSpirvShader` calls `AssetSystem::ReadBinary`,
  and the build now compiles `.spv` into the asset root's `shaders/` (was a
  sibling `<exe>/shaders/`), so shaders resolve like every other asset. CWD
  scatter closed onto the user root: `options.json` (`OptionsConfig` via
  `ReadUserText`/`WriteText`), `assisi.log` (`FileSink` path via `ResolveUser`),
  `crash.dmp` (resolved under the user root at startup, cached for the handler).
  New user-root tests in `TestAssetSystem.cpp`: write/read round-trips (text +
  binary), parent-dir creation, escape rejection on read and write, and a clean
  error on a missing file. The read-only `Resolve` and new `ResolveUser` share a
  single `ResolveUnder` spine, so the escape check is defined once.
- [ ] **Inspector component lookup is O(all components of every type) per
  frame.** `DrawInspector` (`main.cpp:667-680`) finds one entity's component
  by calling `meta.iterateEntities` — a full scan of every entity in every
  registered pool — per component type, per frame, to locate a single
  (index, generation) match. The reflection meta needs a direct
  `getByEntity(scene, index, gen) -> const void*` accessor (trivial to emit
  in reflectgen: `scene.Get<T>(Entity{idx, gen})`). The linear scan will get
  copy-pasted into the next tool if it survives.
- [ ] **Hardcoded `D24S8` depth format with no device-support check.**
  `VulkanContext.cpp:369` (`nvrhi::Format::D24S8`), mirrored in
  `DebugUI.cpp:100` (`VK_FORMAT_D24_UNORM_S8_UINT`, with a comment that
  explicitly ties itself to the first hardcode). D24S8 is famously
  unsupported on AMD hardware — this is a guaranteed first external bug
  report. Fix: query `vkGetPhysicalDeviceFormatProperties` (or NVRHI's
  equivalent) at swapchain creation, fall back D24S8 → D32S8 → D32, and
  surface the chosen format so DebugUI/PostProcess consume it instead of
  re-hardcoding.
- [ ] **No validation layers or debug messenger anywhere.**
  `CreateInstance` (`VulkanContext.cpp:27-52`) enables no layers and no
  `VK_EXT_debug_utils`. For a from-scratch Vulkan engine this is developing
  blind — every synchronization or usage error is invisible until it becomes
  a visual artifact or a crash. Fix: in debug builds (or behind an env/config
  flag), enable `VK_LAYER_KHRONOS_validation` when present and install a
  debug-utils messenger that routes to `Core::Log`. A day of work,
  disproportionate payoff.
- [ ] **The frame-timing stack is three layers fighting.** (Acknowledged
  provisional; listed because provisional caps the score.)
  (a) `BeginFrame` does a full `waitForIdle()` (`VulkanContext.cpp:428`,
  documented trade); (b) present mode is hardcoded FIFO — vsync always on at
  the Vulkan level (`VulkanContext.cpp:316`); (c) `Application::Run` calls
  `_window->SetVSyncEnabled(false)` (`Application.cpp:244`) which is a stub
  that stores a bool (`WindowContext.cpp:183-188`), then paces frames itself
  with a busy-wait `SleepUntil` (`Application.cpp:207-223`) whose target
  FIFO then overrides anyway. Resolve the stack into one design: real
  frames-in-flight with fences, present-mode selection wired to options, and
  either delete `SetVSyncEnabled` or make it actually recreate the swapchain
  with MAILBOX/IMMEDIATE. Until then, **delete the stub** — a false
  affordance is worse than an absent one. Also fix the header doc rot:
  `WindowContext.hpp:104-110` still documents the stub as applying a swap
  interval ("Makes the window context current before changing the swap
  interval") — contradicted by the implementation 80 lines below.
- [ ] **`SystemRegistry` duplicates its entire plumbing between game and
  render paths.** `GameEntry`/`RenderEntry`, `_entries`/`_renderEntries`,
  paired dirty flags, and forked `After`/`Before`/`Register`/`Run`
  (`SystemRegistry.cpp:128-212`). The render `Register` overload takes a
  `SystemPhase` argument *solely* to log an error if you pass the wrong one
  (`SystemRegistry.cpp:173-189`) — API design apologizing for itself.
  Fix: a `RegisterRender(name, fn)` without the phase argument (removes the
  runtime check and the apology), and collapse the duplicated storage with a
  small template over the context type.
- [ ] **`Logger` is not thread-safe while multi-threaded code lives
  in-process.** `Logger.cpp` — no synchronization on `_sinks` or in the
  sinks; `ConsoleSink::Write` interleaves on `std::cout`. Jolt's
  `JobSystemThreadPool` threads exist right now; the first `Log::Warn` from
  a physics callback is a data race. Fix: either a mutex in
  `Logger::Log` (cheap, fine at current volume) or an explicit,
  asserted single-thread policy (`std::this_thread::get_id()` check in debug)
  — pick one, document it on the class.
- [ ] **`MeshPass::_bindingSetCache` — carried over from round 1, still
  open.** (`MeshPass.cpp:144-170`.) Keyed on raw `ITexture*`, never evicted:
  unbounded growth, keeps every texture ever drawn alive, and pointer reuse
  returns a stale binding set. Round 1 documented the contract; the real fix
  (invalidation/eviction) is forced the moment per-entity assets exist — see
  "path to 9" below. Also still const-correctness theater: `Draw() const`
  mutating a `mutable` cache.
- [ ] **Per-frame allocation sweep (minor, bundled):**
  `LightingSystem::Update` allocates three `std::vector`s every frame
  (`LightingSystem.cpp:28-30`) — make them members and `clear()`;
  `QueryView`'s iteration does redundant lookups — `HasAll` runs N sparse
  lookups, then `operator*` runs `Get` which internally repeats `Has`
  (`Query.hpp:40-52`) — fine today, but it's the innermost loop of the
  engine; worth one pass when it shows up in a profile. (Do not optimize
  ahead of instrumentation — see "path to 10".)

## Tests / process

- [x] **`SceneSerializer` had zero tests** — now has a suite
  (`modules/Runtime/tests/TestSceneSerializer.cpp`): value round-trip, the two
  forward-reference regressions, empty-scene, transient-only component, multi-
  component, unsupported-version no-op, file round-trip, malformed/missing file,
  and unknown-component skip.
- [ ] **`reflectgen` has zero tests** — a 521-line regex-based C++ parser
  (`tools/reflectgen/reflectgen.py`) that generates engine-critical code,
  completely undefended. Golden-file tests: a fixture header with every
  supported field type + edge cases (nested braces, comments containing
  `ACOMP`, namespaced types, transient fields) → assert the generated .cpp
  matches a checked-in golden output. Cheap to write, catches every future
  regex regression.
- [~] **`PropagateTransforms`/`Hierarchy` had zero tests** — now covered by
  `modules/Runtime/tests/TestHierarchy.cpp` (root, parent-compose, cycle guard).
  *Still open:* deep-nesting chains and explicit memoisation-correctness cases.
- [ ] **No CI.** `.clang-format`, `.clang-tidy`, sanitizer CMake plumbing
  (`ASSISI_ENABLE_SANITIZERS`), and 27 good doctest cases all exist — and
  nothing runs any of them on push. This is the highest-leverage open item
  in the whole review: every consistency defect below is the kind of thing
  a bot catches for free and a human catches never. Minimum viable:
  GitHub Actions with (a) Windows + Linux build (the Linux paths in
  AssetSystem/CMake are currently dead code nobody runs),
  (b) `ASSISI_WARNINGS_AS_ERRORS=ON`, (c) ctest, (d) format check,
  (e) an ASan/UBSan job on the pure-logic test suites (the plumbing is
  already in the root CMakeLists — it's wired to nothing). Stretch: a
  headless render smoke test via lavapipe/SwiftShader so pipeline-desc
  breakage is caught without a GPU.

## Style / consistency / rot

- [x] **`SceneRegistry` allocates a `std::string` per `Get`/`Has`/`Destroy`/
  `SetActive` lookup**. *Fixed:* `_scenes` now uses transparent hash + equality
  so lookups take `std::string_view` without allocating. The hash itself was
  de-duplicated in the process — `TransparentStringHash` moved to a shared
  `Assisi/Core/StringHash.hpp`, and `ActionMap`'s copy (the round-1 original)
  now uses it too, so there is one definition instead of a spreading clone.
- [x] **`_scenes.Create("Main").value()`** (`main.cpp:195`) — `.value()` on
  a failed `expected` throws `bad_expected_access`. *Fixed:* branch on the
  result, log an error, and bail from `OnStart` like every other bring-up
  failure (a null `_scene` is already handled downstream).
- [x] **`QueryView` exposes public members with private naming** —
  `_pools`, `_primary` on a public aggregate (`Query.hpp:27-28`), and the
  nested `Iterator`'s `_entities`/`_pos`/`_pools` likewise. *Fixed:* members and
  constructor are now private, with `Scene` as the sole friend — the view exists
  only to be returned by `Scene::Query`, so `friend` documents coupling that is
  already inherent rather than adding any. (`Iterator` befriends its enclosing
  `QueryView`; a nested-type friend, benign.) This closes the one real
  encapsulation seam behind the `SparseSet::Add` liveness story — the public
  `_pools` was the only way to reach a mutable pool pointer and call
  `Add`/`Remove` around `Scene::Add`'s `IsAlive` gate. Iteration still yields
  mutable `Ts&`, so in-place component writes are unaffected.
- [x] **Dead initializers:** `_yaw = -116.6f; _pitch = -24.1f`. *Fixed:*
  initialized to `0.f` with a note that `SetupCamera()` sets the real values
  before first use.
- [x] **Stale comment:** `VulkanContext.cpp:6` cited the deleted
  `apps/vk_triangle/src/main.cpp`. *Fixed:* the dead reference is gone; the
  comment now points at the Donut framework (`DeviceManager_VK.cpp`) alone.
- [x] **Stale bookkeeping in round-1 doc:** the frames-in-flight item
  (`code-review-2026-07.md:98-103`) was fixed by commit b7ac58c but left
  unchecked. *Fixed:* checked off in the round-1 doc with a pointer to b7ac58c.
- [x] **reflectgen `mat4` is half-implemented TODO codegen**. *Fixed:* `mat4`
  is now actually implemented — serialized as a flat 16-float array in
  column-major order, deserialized through glm::mat4's matching 16-scalar
  constructor (round-trip verified exact against glm, no transpose). Separately,
  the no-stubs gap it exposed is closed: an `UNSUPPORTED_TYPES` guard makes a
  non-transient `AFIELD()` of any deliberately-unimplemented type a hard
  generation error (with a clear message) instead of a silent null round-trip —
  currently empty since every recognised type is supported, but ready for the
  next one. Verified: real headers and a `mat4` fixture generate; a fixture of a
  guarded type exits non-zero.
- [x] **Inspector can't edit `EntityRef` fields** — `FieldType::EntityRef`
  fell into the `default:` "[unsupported type]" branch of `EditComponentFields`.
  *Fixed:* it now has a real editor with two ways to set the target:
  (1) a **dropdown** listing every live entity in the scene (plus `(none)`),
  with the current value previewed and marked `(dangling)` if the stored handle
  is stale; and (2) an **eyedropper** — a "Pick" button that arms a mode where
  the next left-click in the 3D scene writes the picked entity into the field
  instead of moving the selection (re-using the existing `PickEntity` raycast).
  The armed target is pinned by (entity, component meta, field offset) rather
  than a raw component pointer, so a pool reallocation between arming and picking
  can't dangle it; the write re-resolves the pointer through `iterateEntities` at
  pick time. Backed by two new ECS tooling primitives — `Scene::EntityAt(index)`
  (slot → current-generation handle, or `NullEntity`) and
  `Scene::ForEachEntity(fn)` / `Registry::ForEachLive(fn)` (visit every live
  entity, skipping freed slots) — each covered by regression tests in
  `TestRegistry.cpp` (live-slot resolution, freed-slot-follows-reuse proving no
  stale handle escapes, and ForEachLive skipping a mid-array hole then following
  slot reuse).
- [x] **Triplicated rationale:** the cluster-rebuild-on-projection-drift
  story is now told once, on the method (`RebuildClusterGrid`'s doc); the
  member (`_clusterProjection`) and the call site both point at it instead of
  re-telling it. *(Already de-duplicated when the cluster-rebuild fix landed;
  verified and checked off here.)*
- [x] **Residual narration comments** — swept: `SparseSet.hpp` lost the
  narrate-the-obvious lines in `Add`/`Remove` (the "grow the sparse array",
  "move the last element", "clear the sparse entry" trio), keeping only the
  why-comments (the stale-slot rejection rationale and the moved-entity sparse
  re-point); `AssetSystem.cpp` lost the `gInitialized` restatement and the
  "allocate exact size" narration, keeping the why-comments (seek-to-end sizing,
  `tellg` failure, text-vs-binary EOF handling, `_dupenv_s` free).
- [ ] **`main.cpp` at 848 lines is at the ceiling.** Well-sectioned, but the
  inspector (`EditComponentFields`/`DrawInspector`/`HandlePhysicsEditing`)
  and level management (`ScanLevels`/`LoadLevel`/`SaveLevel`) are each a
  file's worth of code that will grow. Split when next touched; don't let it
  hit 1200.

## What's good (don't regress it)

Everything round 1 listed still holds, plus specifically earned since:
`WindowContext`'s callback-ownership design is the correct fix done properly
(single owner, trampolines, ImGui chaining, move-safety re-seating the user
pointer); the error-handling convention is real and mechanically enforced
with `[[nodiscard]]`; `PhysicsWorld`'s pImpl + refcounted Jolt globals is
textbook; the 27 existing test cases are behavioral tests, not line-coverage
filler; comment discipline is overwhelmingly why-comments of genuinely high
quality (Y-flip, winding order, opener pipeline, waitForIdle rationale);
the engineering-memory practice (migration doc, burn-down review docs,
gotcha comments pointing at docs) is better than most professional teams.

## Scoring and the path up

**7/10 now.** The round-1 6 → 7 delta is the completed burn-down; the cap is
the two spine bugs + the provisional surfaces + no enforcement.

**To 8 — fix this document's bug list.** Items under "Correctness bugs" plus
GameApplication resolution, asset-path unification, and CI. Mechanical; no
design work required beyond the two-pass loader.

**To ~9 without building more engine — make the current scope honest and
defended.** A small engine can be a 9; a stubbed one can't:
1. Delete or hard-fail every stub (`SetVSyncEnabled`, reflectgen mat4).
2. Enforce contracts instead of documenting them: structural-change
   assertions in `QueryView` (per-pool modification counter checked by the
   iterator in debug builds), stale-handle asserts in ECS debug paths.
3. Test the two components whose failure mode is *silent*: SceneSerializer
   (adversarial round-trip) and reflectgen (golden files).
4. CI with sanitizers and the format/tidy gates wired to the configs that
   already exist.
5. The small honesty items: thread-safe (or thread-asserted) logger,
   validation layers in debug, one asset-resolution scheme, one CWD policy.

**To a true 9–10 — evidence, not polish.** Abstraction quality is empirical,
and three core abstractions have one consumer each. The remaining gap closes
only through load-bearing use:
- **Per-entity asset references in the scene format.** `.alvl` currently
  stores no asset paths — `LoadLevel` hardwires every `MeshRendererComponent`
  to the one cube and one checker texture (`main.cpp:736-740`). Real asset
  references force the deferred designs to actually exist: an asset cache
  with a lifetime policy (which finally forces the `MeshPass` binding-set
  eviction), per-entity load-failure handling, a serializer carrying real
  content. Most "wrong the day asset streaming exists" caveats in the
  codebase resolve through this one feature.
- **A designed frame architecture:** real frames-in-flight with fences,
  present-mode selection, the physics/render/vsync timing story reconciled
  into one coherent design instead of three layers fighting.
- **A second consumer** for the App layer / reflection / SystemRegistry
  (port sandbox onto GameApplication now; editor or a minimal game later).
  Interfaces only earn their shape under two masters.
- **Performance work driven by measurement:** frame/GPU timestamp
  instrumentation and a stats overlay *first*, then culling/sorting/
  allocation work justified by profiler data. A 10 doesn't guess.
- **An explicit threading model:** single-threaded-except-Jolt today, with
  nothing asserting who may touch what. Document-and-assert, or parallelize
  deliberately.
- **Owned failure modes:** device-lost recovery, mid-frame asset failure,
  corrupt-save recovery — the paths only shipping forces.

10 is asymptotic: it's what the codebase looks like after it has shipped
something and absorbed the scar tissue. The floor, meanwhile, stays pinned by
the principles at the top of this doc — no new promissory notes, contracts
enforced on landing, risky paths tested on landing. The score follows the
debt ledger, not the feature count.
