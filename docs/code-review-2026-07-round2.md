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

- [ ] **`SparseSet::Add` with a stale handle corrupts the live occupant's
  mapping.** `SparseSet.hpp:44-59`. The round-1 generation fix protected
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
- [ ] **`SceneSerializer::Load` breaks forward entity references — silent
  hierarchy loss on round-trip.** `SceneSerializer.cpp:142-159`. Load creates
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
- [ ] **`PropagateTransforms` has no cycle guard — parent cycle = stack
  overflow.** `Hierarchy.cpp:27-42`. The `worldMatrix` lambda recurses through
  `std::function` with memoisation but no in-progress detection. A parent
  cycle (A→B→A) in a hand-edited `.alvl` file recurses until the stack dies.
  Level files are exactly the data that will eventually contain one; the
  serializer will happily round-trip a cycle today. Fix: track an
  "in-flight" set (or a visiting flag in the cache with a sentinel value);
  on re-entry, log an error naming the entity and treat it as a root.
  Secondary: the function allocates a fresh `unordered_map` + closures every
  call, and it runs twice per frame (`main.cpp:320-321`, camera scene +
  main scene). Keep a scratch map as a member/static or pass one in.
- [ ] **`~VulkanContext` leaks the instance and surface on partial init.**
  `VulkanContext.cpp:487-491`. The destructor early-returns when
  `_device == VK_NULL_HANDLE`, but `Create()` can fail *after* the instance
  and surface exist (`ChoosePhysicalDevice` or `CreateLogicalDevice` failure,
  `VulkanContext.cpp:190-208`) — those paths return nullptr, the partially
  built context is destroyed, and `vkDestroySurfaceKHR`/`vkDestroyInstance`
  never run. Cosmetically small (process is exiting), but it half-applies the
  "failures unwind normally" convention that commit 587b8e2 established.
  Fix: guard each teardown on its own handle instead of gating everything on
  `_device`.
- [ ] **`HandlePhysicsEditing` keys off `ImGui::IsAnyItemActive()` globally —
  any widget interaction anywhere freezes the selected entity's physics.**
  `main.cpp:616`. `IsAnyItemActive()` is true while dragging the AA combo,
  typing in the Save-As field, or using any other window's widgets — and the
  handler responds by setting the selected entity's body to `Static`
  (`main.cpp:626-629`) until the widget is released. Fix: scope the check to
  the inspector's own widgets — capture `ImGui::IsAnyItemActive()` *inside*
  the Inspector window's Begin/End (or track activity only on the component
  field widgets `EditComponentFields` actually drew).

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
- [ ] **Asset resolution is two systems; shaders bypass `AssetSystem`
  entirely.** Textures and levels resolve through `AssetSystem::Resolve`
  (root discovery, escape protection); shaders are raw CWD-relative
  `std::ifstream` (`ShaderModule.cpp:16-30`, paths like
  `"shaders/cube_min.vert.spv"`), depending on the POST_BUILD copy next to
  the exe (`apps/sandbox/CMakeLists.txt:88-89`) *and* on the process being
  launched from that directory. Run the exe from any other CWD: textures
  load (AssetSystem walks up from the exe dir), shaders fail. Related CWD
  scatter: `options.json` read/written to CWD (`OptionsConfig.cpp`),
  `assisi.log` to CWD (`Application.cpp:116`), `crash.dmp` to CWD
  (`Application.cpp:51`). Fix: one filesystem story — either shaders resolve
  through AssetSystem (compiled `.spv` under the asset root or a parallel
  runtime-data root), or there is an explicit, documented "runtime dir =
  exe dir" rule applied consistently, including the writable outputs.
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

- [ ] **`SceneSerializer` has zero tests** — and it's the subsystem carrying
  a real data-loss bug (forward refs, above). Needed: round-trip tests
  (save → load → compare), adversarial input (child-before-parent fixture,
  unknown component names, truncated/malformed JSON, wrong version),
  hierarchy preservation. All pure-logic, no GPU required — it fits the
  existing doctest/CTest infra exactly.
- [ ] **`reflectgen` has zero tests** — a 521-line regex-based C++ parser
  (`tools/reflectgen/reflectgen.py`) that generates engine-critical code,
  completely undefended. Golden-file tests: a fixture header with every
  supported field type + edge cases (nested braces, comments containing
  `ACOMP`, namespaced types, transient fields) → assert the generated .cpp
  matches a checked-in golden output. Cheap to write, catches every future
  regex regression.
- [ ] **`PropagateTransforms`/`Hierarchy` have zero tests.** Parent chains,
  deep nesting, the cycle guard once it exists, memoisation correctness.
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

- [ ] **`SceneRegistry` allocates a `std::string` per `Get`/`Has`/`Destroy`/
  `SetActive` lookup** (`SceneRegistry.cpp` — `_scenes.find(std::string(name))`,
  `contains(std::string(name))`). This is the exact defect round 1 fixed in
  `ActionMap` with `TransparentStringHash`. The convention was established
  and then not swept across the codebase. Apply the same transparent
  hash/equality to `SceneRegistry::_scenes`.
- [ ] **`_scenes.Create("Main").value()`** (`main.cpp:195`) — `.value()` on
  a failed `expected` throws `bad_expected_access`, in a codebase whose
  signature round-1 achievement was the no-exceptions bring-up convention.
  Branch and bail like every other bring-up failure.
- [ ] **`QueryView` exposes public members with private naming** —
  `_pools`, `_primary` on a public aggregate (`Query.hpp:27-28`), and the
  nested `Iterator`'s `_entities`/`_pos`/`_pools` likewise. Either make them
  private with a constructor, or drop the underscore prefix. As-is the
  naming lies about the access level.
- [ ] **Dead initializers:** `_yaw = -116.6f; _pitch = -24.1f`
  (`main.cpp:129-130`) — magic values unconditionally overwritten by
  `SetupCamera` (`main.cpp:152-153`). Initialize to 0 or drop the
  initializers; the current values send a reader hunting for meaning that
  isn't there.
- [ ] **Stale comment:** `VulkanContext.cpp:6` cites
  `apps/vk_triangle/src/main.cpp` as "the original proof of this pattern" —
  that app was deleted (round 1 removed `apps/editor`; `vk_triangle` is gone
  too). Point at the Donut reference alone, or at this file's own history.
- [ ] **Stale bookkeeping in round-1 doc:** the frames-in-flight item
  (`code-review-2026-07.md:98-103`) is still unchecked but was fixed by
  commit b7ac58c ("collapse vestigial frames-in-flight machinery to one
  semaphore pair"). Check it off — the burn-down doc only works if it's
  trustworthy.
- [ ] **reflectgen `mat4` is half-implemented TODO codegen**
  (`reflectgen.py:113-116`): serialize emits `nullptr /* TODO */` (a `mat4`
  field silently round-trips as JSON null), deserialize emits a comment.
  Per the no-stubs principle: make an `AFIELD()` on a `glm::mat4` a hard
  generation *error* until it's actually supported. Silent-null is the worst
  of the three options.
- [ ] **Inspector can't edit `EntityRef` fields** — `FieldType::EntityRef`
  falls into the `default:` "[unsupported type]" branch of
  `EditComponentFields` (`main.cpp:585-587`). Minor today; note it so the
  gap is chosen rather than discovered.
- [ ] **Triplicated rationale:** the cluster-rebuild-on-projection-drift
  story is told three times — member doc (`main.cpp:120-124`), method doc
  (`main.cpp:78-82`), call site (`main.cpp:335-337`). Tell it once where the
  decision lives (the method), point at it from the other two. Same review
  note as round 1's "why-comments are the model" — with the addendum that
  even good comments shouldn't be cloned.
- [ ] **Residual narration comments** in `SparseSet.hpp` ("Append the entity
  index and the component value" above `push_back`) and a few in
  `AssetSystem.cpp` — round 1's strip pass got most; sweep the rest.
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
