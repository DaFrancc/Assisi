# Codebase review — 2026-07-07

Full first-party review (~9.7k lines: all of `modules/`, `apps/sandbox`, build
system, shaders spot-checked) done right after the NVRHI migration closed out.
Scored **6/10** — strong skeleton (layering, CMake, docs discipline), dragged
down by zero tests, a latent ECS correctness bug, singleton coupling, and
consistency rot. Items below are ordered by severity within each section;
checkboxes so this can be burned down like the migration doc was.

## Conventions decided during this review

- **Component types do NOT need a `Component` suffix.** Repetitive
  (`TransformComponent`, `CameraComponent`, ...). New components should use the
  bare name (`Transform`, `Camera`, `RigidBody`, ...); existing ones can be
  renamed opportunistically. The `ACOMP()` annotation already marks what's a
  component — the suffix carries no information.

## Correctness bugs (real, not taste)

- [x] **`SparseSet` ignores entity generations — stale handles alias new
  entities.** `SparseSet::Has()` (`SparseSet.hpp:88`) compares only
  `entity.index`. Destroy A → `Create()` reuses the index for B → `Get<T>(staleA)`
  returns **B's component** and `Has(staleA)` is true. `Registry::IsAlive()` is
  the only defense and call sites are inconsistent about using it. Fix: check
  `_entities[pos].generation == entity.generation` in `Has` (or store the full
  Entity in the sparse array). **A unit test must land with the fix.**
- [x] **Structural changes during `Query` iteration are silent UB and
  undocumented.** (Contract now documented on `Scene::Query`; points at the
  EventQueue for deferred destruction.) `QueryView` holds a raw pointer into the primary pool's
  `_entities` vector (`Query.hpp:27`); `Remove`/`Destroy`/`Add` mid-loop
  swap-removes or reallocates under the iterator. At minimum, document the
  contract on `Scene::Query`; the `EventQueue` exists precisely to defer
  destruction — point at it.
- [ ] **GLFW callback ownership is a three-way collision.** `Application.cpp:77`
  avoids `glfwSetWindowUserPointer` because ImGui's backend uses it — yet
  `InputContext`'s ctor (`InputContext.cpp:19-20`) sets both the user pointer
  *and* the scroll callback, **after** `DebugUI::Initialize` installed ImGui's
  chained callbacks. InputContext's callback replaces ImGui's, so ImGui
  plausibly never receives scroll events; correctness currently depends on
  undocumented constructor ordering. Centralize GLFW callback ownership in
  `WindowContext` and have everyone subscribe through it.
- [x] **`PhysicsWorld` does process-global Jolt init with instance semantics**
  (Refcounted acquire/release of the Jolt globals; also fixed the dtor tearing
  down the factory before the PhysicsSystem that depends on it.)
  (`PhysicsWorld.cpp:132-157`). Second instance leaks the factory; destroying
  either instance breaks the other. Guard it (refcount/once) or document and
  assert "at most one".
- [x] **Swallowed error in input-binding load**: `SandboxApp::OnStart`
  (`main.cpp:174`) `catch (json::exception) {}` — malformed `game.json`
  silently loses all input bindings. Log it, at minimum.
- [ ] **`Application` ctor is the whole engine bring-up and `std::exit()`s on
  failure** (`Application.cpp:114-165`) — skips every destructor (no
  `vkDeviceWaitIdle`, no cleanup), untestable. Wants `Initialize() -> bool` or
  exceptions.
- [x] **`VulkanContext` robustness gaps**: `formats[0]` indexed without
  checking count; `vkCreateSemaphore`/`vkQueuePresentKHR`/surface-capability
  results ignored; `ToNvrhiFormat` silently returns `UNKNOWN` for anything but
  two formats and nothing checks it. (All now checked/guarded; present result
  distinguishes expected OUT_OF_DATE/SUBOPTIMAL from real errors.)

## Architecture

- [x] **Zero tests anywhere.** Test infra landed: doctest (header-only,
  `SOURCE_SUBDIR` trick to skip its old CMakeLists) + CTest, gated behind
  `ASSISI_BUILD_TESTS`, per-module under `modules/<Name>/tests/`. Covered:
  **ECS** `SparseSet`/`Registry`/`Query` (15 cases, incl. the generation-aliasing
  regression), **Core** `AssetSystem::Resolve`/`NormalizeVirtualPath` (via public
  API), **App** `SystemRegistry::TopoSort` (ordering/missing-dep/cycle via render
  phase), **Window** `ActionMap` JSON round-trip. 27 cases / 115 assertions, all
  four `ctest` suites green.
- [ ] **Singleton sprawl**: `EventQueue::Instance()`, `ComponentRegistry::
  Instance()`, `AssetSystem` file statics, `RenderSystem::GetVulkanContext()`,
  `DebugUI` statics, `s_instance`. Individually defensible; collectively the
  reason nothing is testable and why ordering contracts are hidden. Services
  the frame loop touches should be reachable as members, not ambient globals.
- [~] **`MeshPass::_bindingSetCache` trap** (`MeshPass.cpp:144-170`): keyed on
  raw `ITexture*`, never evicted — every texture ever drawn is kept alive
  forever, and pointer reuse returns a stale binding set. Harmless with one
  checker texture; wrong the day asset streaming exists. **Contract now
  documented** on the field (both hazards + the fix). Invalidation still TODO
  when real assets land. (Also: `Draw() const` + `mutable` cache is
  const-correctness theater.)
- [ ] **Vestigial frames-in-flight machinery** in `VulkanContext`:
  `kFramesInFlight` semaphore arrays + `_frameIndex` while `waitForIdle()` in
  `BeginFrame` guarantees zero overlap; `_renderFinishedSemaphores` is indexed
  per-frame rather than per-swapchain-image (masked only by that wait). Either
  commit to the simple design (one semaphore pair) or pipeline properly with
  fences.
- [ ] **Error-handling is four dialects**: `std::expected` (Core), `bool`+log
  (Render), log-and-continue with results ignored (VulkanContext), `std::exit`
  (Application). Pick a convention per layer and enforce it.

## Dead code / rot

- [x] `modules/Runtime/Transform.hpp` — 108-line `Transform` class, zero
  references. Delete (`TransformComponent` won).
- [x] `modules/Window/Window.hpp` + `Window.cpp` — `void Hello()` scaffolding,
  still compiled into the lib. Delete. (Also removed the identical
  `modules/ECS/ECS.hpp` + `ECS.cpp` `Hello()` stub.)
- [x] `Application::MakeProjection` — zero callers. Delete.
- [x] `apps/editor` — no `main.cpp`, not in the build, still accumulating
  half-maintained CMake. Deleted.
- [x] Stale comment `main.cpp:326-328`: claims ImGui is OpenGL-only — false
  since the ImGui Vulkan port landed.
- [x] `DrawLevelsWindow` says "No .json files found" but `ScanLevels` scans
  `.alvl` (`main.cpp:438` vs `674`).
- [x] Garbled doc sentence `Application.hpp:36-39` ("called whenever the
  FramebufferInfo OnRender()'s `frame` will be compatible with next changes").

## Style / consistency

- [x] Copyright headers on ~half the files (`Registry.cpp` yes, `Scene.hpp` no;
  `PostProcess.cpp` yes, `VulkanContext.cpp` no). One policy, enforced. (Header
  now on all 91 first-party files.)
- [x] `override` on 3 of 7 overridden virtuals in `SandboxApp`
  (`main.cpp:60-66`). All or nothing. (All 7 now marked; clang-tidy
  `modernize-use-override` was already active via `modernize-*` in `.clang-tidy`.)
- [x] `AssetSystem.cpp` narrates the obvious (`/* Resolve the asset path. */`
  above `Resolve(vpath)`, ~30 instances) — strip; keep only why-comments. (The
  Render/Vulkan why-comments — Y-flip, winding order, waitForIdle, POST_BUILD —
  are the model to follow.)
- [x] `ActionMap` allocates a `std::string` per action query per frame
  (`_actions.find(std::string(action))`, ×3 methods) — transparent comparator.
  (Added `TransparentStringHash` + `std::equal_to<>` heterogeneous lookup.)
- [x] `SystemRegistry::Register` render overload takes a `SystemPhase` and
  ignores it (`SystemRegistry.cpp:166`); `Log::Fatal` on a dependency cycle
  then keeps running with zero systems — a "Fatal" that isn't. (Render phase is
  now validated; cycle falls back to registration order at Error level so no
  systems are silently dropped.)
- [x] `AssetSystem::Resolve` root-escape check is a `starts_with` prefix test
  (`AssetSystem.cpp:86`) — `assets-evil/` would pass. (Now uses
  `lexically_relative` + `..` component check.)
- [x] `Scene::Add` copies the component twice (by-value param, then
  `emplace_back(component)` without move). (Both `Scene::Add` and
  `SparseSet::Add` now move.)
- [x] `PickEntity` divides by `h` unguarded (`main.cpp:774`) while the other
  three aspect computations guard `height > 0`.

## What's good (don't regress it)

Clean dependency-ordered module layering with no cycles; CMake hygiene
(interface targets for options/warnings/sanitizers, pinned deps, warnings on by
default, the reflection OBJECT-library workaround); pImpl in `PhysicsWorld`;
`std::expected` in Core; exceptional engineering-memory discipline in the
migration doc and gotcha comments; `SparseSet`/`Query` are the right data
structures — they just need the generation check and tests.
