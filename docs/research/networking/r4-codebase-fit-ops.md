# R4 — Codebase Fit & Operational Concerns for Assisi Networking

**Scope:** Audit the Assisi codebase against the staged networking plan in
`docs/networking-design-notes.md` (Stages N0–N7), cross-checked against web
research on transport/frame-loop/ops best practices. Primary source is the code;
web sources qualify integration and operational claims.

**Method note:** All codebase line numbers below were re-verified against the
working tree on branch `asset-upgrade`. The design doc's own citations have
drifted a few lines (it was written 2026-07-20 against an earlier revision):
e.g. it cites `Application::Initialize()` at `Application.cpp:148-206` but the
function now spans **149–218**; the fixed-step loop it cites at `311-316` is now
**324–328**; `SleepUntil` cited at `247` is now **259–282**. The drift is
cosmetic — every structural claim the doc makes still holds — but anyone
executing the plan should re-grep rather than trust the embedded line numbers.

---

## PART A — Codebase Audit

### A1. modules/App — lifecycle, frame loop, and the headless split (Stage N2)

**What the loop looks like today.** `Application` is a single concrete base
class (`modules/App/include/Assisi/App/Application.hpp`) with pure-virtual sim
hooks (`OnStart`, `OnFixedUpdate`, `OnUpdate`, `OnRender` — all `= 0` at
`Application.hpp:73-76`) and optional no-op hooks (`OnImGui`, `OnShutdown`,
`OnResize`, `OnRenderTargetsChanged`, `FlushDeferred`).

`Initialize()` (`Application.cpp:149-218`) is a straight-line bring-up with **no
headless branch**:

1. `Core::AssetSystem::Initialize()` (line 156) — sim-side, render-free.
2. `AppConfig::LoadFromJson()` (line 173) — sim-side.
3. `WindowContext` construction + validity check (lines 180-185) — **presentation**.
4. Framebuffer/refresh callbacks wired to the window (lines 191-192) — presentation.
5. `Render::RenderSystem::Initialize(*_window)` (line 194) — presentation; the
   render system is a **static singleton**, not an owned member.
6. `Debug::DebugUI::Initialize(...)` (line 200) — presentation.
7. `Window::InputContext` construction (line 202) — presentation.
8. `OptionsConfig::LoadFromJson()` (line 203) — sim-side (but only meaningful
   with a display).
9. `PostProcess::Initialize(...)` + `ConfigurePostProcess()` (lines 205-214) — presentation.

**The run loop** (`Run()`, `Application.cpp:285-423`):

- Guarded by `if (!_initialized)` (line 287).
- Loop condition is `while (!_window->ShouldClose())` (line **313**) — a raw
  unconditional `_window` dereference. This is the single hardest dependency to
  unwind for headless: the loop's termination test *is* the window.
- Per iteration: `WindowContext::PollEvents()` (320), `_input->Poll()` (321),
  the fixed-step accumulator `while (accumulator >= physicsStep)` calling
  `OnFixedUpdate` (324-328), `_interpolationAlpha` computation (334),
  `_jobs.DrainMain()` (340), `OnUpdate` (342), FPS-limit pacing via `SleepUntil`
  (352), a `SetVSync` reconcile (367), `RenderFrame()` (370), `_events.Flush()`
  (371), `FlushDeferred()` (372), then frame-time accounting.

**The fixed-step block is already presentation-independent.** Lines 324-328 read
only `accumulator`, `physicsStep`, and call `OnFixedUpdate(float)`. Nothing in
that block touches the window, renderer, or input. The design doc's claim (§Stage
N2) that "the fixed-step accumulator block is unchanged — it is already
presentation-independent" is **verified correct**. This is the crux that makes
N2 tractable: the simulation clock the whole netcode plan hangs off already lives
in a render-free region of the loop.

**What actually depends on Window/Render/Input** (the things a headless path must
skip or the destructor must guard):

| Concern | Site | Headless disposition |
|---|---|---|
| Loop termination | `_window->ShouldClose()` (313) | Replace with a `_closeRequested` flag; `RequestClose()` currently calls `_window->RequestClose()` (`Application.cpp:237-240`) and would need to set the flag instead. |
| Event pump | `WindowContext::PollEvents()` (320) | Skip. |
| Input | `_input->Poll()` (321) | Skip; `_input` stays null. |
| Frame pacing | `SleepUntil(nextRenderTime)` gated on FpsLimit (349-356) | Repurpose `SleepUntil` to pace to the next **fixed tick** instead of a render deadline (the function itself, 259-282, is presentation-agnostic — it only takes a `time_point`). |
| Render | `RenderFrame()` (370) → `RenderSystem::GetVulkanContext()` | Skip; `RenderSystem::Initialize` never called, so `GetVulkanContext()` returns null. |
| GPU drain on exit | `vulkanContext->GetDevice()->waitForIdle()` (419-420) | Already null-guarded via `if (auto *vulkanContext = ...)`. |
| Destructor teardown | `~Application` (220-235) DebugUI/PostProcess/RenderSystem shutdown | Currently gated on `_initialized`; must re-gate on a separate `_presentationInitialized`. |

**Verdict on the proposed split.** The doc proposes splitting `Initialize()` into
`InitializeCore()` (asset/config/jobs/scene) + `InitializePresentation()` (window,
render, DebugUI, input, post-process), headless running only the former. **This is
the right seam and it is a small refactor** — the presentation calls are already
contiguous (window through post-process, lines 180-214) with only `AssetSystem`
and `AppConfig` ahead of them. The estimate of "independently valuable, lands
before any protocol work" holds.

**Is a separate headless-loop class better?** I considered the alternative of a
distinct `HeadlessLoop` / `ServerApp` that does *not* inherit the windowed
`Application`. Assessment: **the in-place split is better than a separate class**,
for three reasons the codebase makes concrete:

1. The sim hooks (`OnFixedUpdate`/`OnUpdate`/`FlushDeferred`) and the fixed-step
   accumulator are the contract both windowed and headless apps share. A separate
   class would have to duplicate the accumulator loop (324-334) verbatim or hoist
   it into a shared helper anyway — at which point you have the split, plus a
   second type.
2. `SystemRegistry` (below) is already the phase dispatcher both would drive; it
   is presentation-agnostic except for two fields.
3. The listen server (Stage N6) runs an *embedded* server inside the *windowed*
   client process — "one scene, not two". A separate server class would force
   that embedding to instantiate a second lifecycle object inside the client;
   the flag-on-one-class model lets the host reuse its own loop and simply also
   run `ReplicationServer`.

**One real snag the doc under-weights: `SystemContext` hard-codes input.**
`SystemContext` (`SystemRegistry.hpp:48-55`) bundles `Window::InputContext&` and
`Window::ActionMap&` by reference:

```cpp
struct SystemContext {
    ECS::Scene            &scene;
    float                  dt;
    Window::InputContext  &input;
    Window::ActionMap     &actions;
    Core::EventQueue      &events;
};
```

`SystemRegistry.hpp` even `#include`s `<Assisi/Window/ActionMap.hpp>` and
`<Assisi/Window/InputContext.hpp>` (lines 34-35), so the game-logic scheduler
transitively depends on the Window module. A headless server has **no
InputContext and no ActionMap** — it consumes `InputCommand` frames off the wire
instead (Stage N3). Two consequences the plan should absorb explicitly:

- Every `Run(phase, {scene, dt, input, actions, events})` call site needs a valid
  `input`/`actions` reference. Headless has neither. The clean fix is what Stage
  N3 already implies: **route player input through the `SystemContext` as
  `InputCommand`, not as raw `InputContext&`** — but that means either (a)
  replacing the two Window fields with an input abstraction that both a live
  `ActionMap` and a networked command queue can satisfy, or (b) making them
  pointers that are null on the server and asserting no system dereferences them
  headless. Option (a) is the honest fix and it is a Stage-N3-adjacent API change
  the doc currently frames only as "gameplay systems migrate off direct
  `input.IsKeyDown`" — it is actually also a `SystemContext` **shape** change and
  a `SystemRegistry`→Window **link** change.
- `SystemRegistry` linking Window is itself a headless-DAG smell (see A5): a
  render-free server that runs game systems still drags `Assisi::Window` in
  through `App` today. Window is GLFW; whether that link is load-safe on a
  headless box is the same open question as the Render link.

**Bottom line A1:** N2 is a genuinely small, correct refactor for the *loop and
lifecycle*. The unstated work is the `SystemContext`/`SystemRegistry` input
coupling, which N3 must own jointly with N2.

---

### A2. modules/ECS — change detection is real; Query is the single hole

This is the most important finding in the audit, because the design doc frames
the Stage-5 change-tick issue more pessimistically than the code warrants.

**A full per-component change-tick system already exists.** It is not something
to build — it is something with **one documented hole**:

- `SparseSet<T>` carries a parallel `_changeTicks` lane (`SparseSet.hpp`),
  enabled per-pool via `SetTracksChanges(bool)` (172-179). `Stamp(entity, tick)`
  (186-190) writes a per-entity last-written tick; `ChangeTick(entity)` (194-199)
  reads it. The lane is kept in lockstep with the dense array across swap-remove
  (94-96) and there is a test proving that alignment survives removal
  (`TestChangeDetection.cpp:98-116`). Untracked pools keep `_changeTicks` empty,
  so tracking is genuinely zero-cost-per-instance when off.
- `Scene` drives a monotonic `_changeTick` counter (`Scene.hpp:375`), bumped on
  every mutable access. `Add<T>` stamps a fresh component (137-138, "a fresh
  component counts as changed"). **`GetMut<T>` stamps** (157-166). **`Get<T>`
  (non-const) deliberately does *not* stamp** (147-151, with an explicit doc
  comment: "writing through this pointer on a tracked type is a silent missed
  change"). `MarkChanged(entity, ComponentId)` (219-223) is the type-erased path
  for the reflected/inspector writers. `Changed<T>(entity, sinceTick)` (210-213)
  is exactly the delta predicate the doc's `ReplicationServer` wants
  (`Changed<T>(entity, lastAckedTick)`).
- Tracking is opt-in via `ACOMP(tracked)`, wired by `Scene` at pool creation
  (`Scene.hpp:164` calls `pool->Stamp(...)`; the `StampFn` type-erased stamp is
  registered at 315-317).
- There is a dedicated test file, `TestChangeDetection.cpp`, covering Add/GetMut
  stamping, Get *not* stamping, the `Changed(since)` transition, `MarkChanged`,
  the untracked control case, and swap-remove alignment. **The mechanism is
  tested and shipped.**

**The actual hole is `Query`.** `QueryView::Iterator::operator*` returns
`std::tuple<Entity, Ts&...>` — **raw mutable references straight into pool
storage** (`Query.hpp:62-65`), and the class comment openly says "the component
references are mutable so systems can write component data in place"
(`Query.hpp:48-51`). No stamp is issued on deref, on `operator++`, or anywhere in
the iterator. So the canonical hot-path pattern

```cpp
for (auto [e, pos, vel] : scene.Query<Position, Velocity>())
    pos.x += vel.x;   // mutates Position; if Position is tracked, NO delta is produced
```

silently defeats change detection for any tracked component written through it.
The design doc's "change-tick landmine" description is therefore **precise but
its framing understates the surrounding maturity**: it is not that "change
detection needs building," it is that *one iteration path bypasses an otherwise
complete, tested system.* That materially lowers the Stage-5 risk.

**Cheapest sound design to close the hole.** Three options, ranked:

**(a) A distinct stamping query — `QueryMut<Ts...>` yielding write-proxies
(recommended).** Add a parallel query whose `operator*` yields, for each required
type, a lightweight `Mut<T>` proxy instead of a bare `T&`. `Mut<T>`'s
non-`const` `operator*`/`operator->` stamp the owning pool for the current entity
on first mutable deref (Bevy's `Mut<T>`/`DerefMut` model); `const` access does
not stamp. Precision: stamps on **actual mutable deref**, not merely on
iteration, so a `QueryMut` that only reads some entities does not over-report
them. Invasiveness: **moderate but contained** — the existing `Query`
(`Query.hpp`) is untouched and keeps serving read paths and untracked writes;
only systems that *write tracked/replicated components* migrate to `QueryMut`,
which is exactly the "player-controlling systems only" set the doc scopes. The
proxy needs a back-pointer to the `Scene` (for the `++_changeTick` bump) or to
the pool plus a tick source; the iterator already holds the required `SparseSet<T>*`
tuple (`Query.hpp:161`), so wiring is local to `Query.hpp` + a small `Mut<T>`
header. Call-site cost: `pos.x += vel.x` becomes `pos->x += vel->x` (or `(*pos).x`),
a mechanical edit confined to migrated systems.

**(b) A blanket "stamp every required tracked pool on deref" variant.** Simpler
to write than (a) — no proxy type, the iterator just calls `Stamp` for each
tracked required pool in `operator*`. **Rejected as the primary:** it stamps
*every iterated entity every frame*, so every replicated entity lands in every
snapshot and delta compression evaporates — the exact opposite of what Stage 5
needs. Acceptable only as a debug/temporary crutch.

**(c) GetMut discipline (the doc's option b).** Forbid tracked-component writes
through `Query`; require `GetMut`/`MarkChanged`. Cheapest to *implement* (zero ECS
change) but relies on human discipline against the single most ergonomic API in
the engine; the natural `for (auto [e, c] : Query<C>()) c.field = ...` is a
silent-desync trap. A debug assert can't easily catch "you wrote through this
reference" (the reference escapes the ECS). **Weakest guarantee.**

**Recommendation:** ship **(a)**. It preserves the fast read path, isolates the
cost to the systems that need it, mirrors a battle-tested design (Bevy), and
turns "silent desync" into "compile-time you-used-the-wrong-query." Estimate:
~150–250 LOC in `Query.hpp` + a `Mut<T>` header + migrating the handful of
player/replicated-write systems. This is a **1–2 day** ECS change, not a research
project — the doc's "leaning (a)" is the correct lean, and the reason it is cheap
is precisely that the stamping substrate (`SparseSet::Stamp`, `Scene::_changeTick`)
already exists and is tested.

**Serializer / ReviveAt / FieldMeta readiness.** The codec Stage 4 depends on
`FieldMeta` (`Core/Reflect/FieldMeta.hpp`), whose `FieldType` enum already
enumerates `Float/Vec3/Quat/Enum/EntityRef/AssetId/AssetIdVector` (15-37) plus a
byte `offset` (63) and enum width/sign metadata (82-83) — everything a
reflection-driven binary walker needs, and it lives in **Core** (render-free),
which is the correct home for a codec both server and client link. `EntityRef`
fields (31) are the ones needing the NetId remap; the JSON serializer's
equivalent remap (`ScopedRawEntityContext`) currently lives in
`Runtime/SceneSerializer.{hpp,cpp}` (confirmed by grep) — the doc's planned
relocation to Core/Reflect or ECS is real and, since `FieldMeta` is already in
Core, **Core/Reflect is the natural landing spot**.

---

### A3. modules/Physics — server rewind fit and the tick model

**Shape.** `PhysicsWorld` is a pimpl over Jolt (`Impl` holds
`JPH::PhysicsSystem physicsSystem`, `PhysicsWorld.cpp:136`), Core+ECS+Jolt only,
**render-free** (`modules/Physics/CMakeLists.txt` links `Assisi::Core`,
`Assisi::ECS`, `Jolt::Jolt`). This is the module NetSync is told to imitate, and
the imitation is apt: Physics is the existing proof that a Core+ECS,
render-free simulation module slots cleanly under App.

**Stepping and tick rate.** Physics is stepped one substep-set per fixed update:
`PhysicsWorld::Update(float)` (`PhysicsWorld.cpp:370-372`) calls
`physicsSystem.Update(dt, collisionSteps, &tempAlloc, &jobSystem)` — a single
Jolt step with `collisionSteps` collision substeps (default 1, clamp 1–16,
`PhysicsWorld.hpp` / `.cpp:125-127,375-377`). It is called from the app's
`OnFixedUpdate`, i.e. inside the `while (accumulator >= physicsStep)` loop at
`Application.cpp:324-328`. Fixed rate is `AppConfig::physicsHz`, **default 60 Hz**
(`AppConfig.hpp:21`), validated `> 0` at load (`AppConfig.cpp:74-79`). So: yes,
physics runs inside FixedUpdate at 60 Hz by default, which is exactly the net
clock Stage N3 wants (`simTick++` per fixed iteration). The two-phase
`Update()` → `CaptureState()` → `InterpolateTransforms(scene, alpha)` split
(`PhysicsWorld.hpp:87,103,118`) is the local interpolation the doc reuses for
remote-entity snapshot interpolation on the client.

**Server rewind (Jolt SaveState/RestoreState) fit.** Grepping the Physics module:
`SaveState`/`RestoreState` are **absent** (no matches). The doc's Stage 7 defers
lag compensation to "Jolt `SaveState`/`RestoreState` exposed through
`PhysicsWorld` (pimpl extension) plus the missing `SetBodyVelocity`." Correction
on one detail: `SetLinearVelocity` **is** already called inside the impl (to zero
velocity, `PhysicsWorld.cpp:520`) and `GetBodyVelocity` is public
(`PhysicsWorld.hpp:127`, `.cpp:475`) — so the Jolt primitive exists; what is
missing is a *public setter*. That is a one-line pimpl passthrough when the time
comes, not new machinery. `SaveState`/`RestoreState` genuinely need new pimpl
methods wrapping `JPH::StateRecorder`. Because it is fully deferred to Stage 7 and
touches only the pimpl, it poses **no architectural risk to N0–N6**; the pimpl
boundary is exactly what makes it a safe late addition (third-party Jolt types
never leak into the module header, so adding `SaveState` is a header-append, not
a header-churn).

**RigidBody is transient — correct for the wire.** `RigidBody` is
`ACOMP(transient)` (`PhysicsComponents.hpp:26`) — a live Jolt body id, never
serialized; `RigidBodyDescriptor` is the `ACOMP()` serializable form
(`PhysicsComponents.hpp:54`). This is precisely the "transient components never on
the wire; each side rebuilds locally from replicated descriptors" pattern the doc
leans on, and `RebuildSceneBodies(scene)` (`PhysicsWorld.hpp:84`) is the rebuild
primitive. The `App::RebindSceneAssetsAndPhysics` composition
(`LevelRuntime.cpp:34-39`) already does exactly the descriptor→transient rebuild
the client will do on snapshot apply. **This half of Stage 5 is de-risked by an
existing, exercised code path** (play-mode restore uses it).

---

### A4. modules/Core — JobSystem, AssetDatabase, headless assets

**JobSystem threading model.** `Pool` has exactly two values — `Worker` and
`Main` (`JobSystem.hpp:59-64`); **there is no `Pool::IO`** (the header's own
"deferred" note lists `Pool::IO` and work-stealing deques as future work,
lines 33-34). The model is "task pool + pinned main-thread tasks" (enkiTS/UE
lineage), with *help-waiting* (a blocking waiter runs queued tasks rather than
sleeping). Main-thread work is marshaled via `RunOnMain` and drained once per
frame by `DrainMain()` at the safe point `Application.cpp:340`.

Implication for network send/receive placement: the doc's decision — **poll GNS
once per frame/tick on the main thread, no JobSystem involvement in v1** — is the
right fit for this model *and* matches how GNS wants to be driven (see B6: GNS
runs its own internal service thread; the app just polls). Putting `Poll()` on a
worker would gain nothing (GNS already threads the wire) and would require
marshaling every `NetEvent` back through `DrainMain` — added complexity for no
win. The doc's stated seam ("if profiling ever shows Poll mattering, it moves to
a worker with results marshaled through DrainMain") is exactly the existing
main-drain mechanism, so the future move is cheap. **No JobSystem change needed
for N0–N6.**

**AssetDatabase and headless assets.** `AssetDatabase` (`Core/AssetDatabase.hpp`)
is the GUID↔path index: `Rebuild()` scans sidecars (`.aast`), `PathFor(id)` /
`IdFor(path)` resolve both directions, and it tracks composite-mesh manifests
(`HasManifest`, `SlotMaterial`). It is **Core-level and render-free** — a
headless server can build and query it without any GPU. The question "do
dedicated servers need assets?" splits cleanly along the module DAG:

- **Collision meshes: yes.** The server owns physics; `RigidBodyDescriptor`
  primitives (box/sphere/capsule/cylinder, `PhysicsComponents.hpp:57-62`) are
  parametric and need no asset load, but any *mesh-derived* collision (convex
  hulls / triangle meshes from imported geometry) requires reading geometry
  data. That path goes through Geometry (Core+Math, render-free) and Core asset
  I/O — **headless-safe**.
- **Textures/materials/GPU meshes: no.** The server never uploads to a GPU. But
  note the subtlety from Stage 5: the server still holds `MeshRenderer`
  *descriptors* (mesh GUID + material GUIDs) because it must **replicate** them
  to clients. Holding the descriptor ≠ loading the texture — the server keeps the
  `AssetId`s as opaque wire values and never resolves them to GPU pointers. So
  the server links whatever module *defines* `MeshRenderer` (see A5) but never
  calls `Render::AssetCache`.

**How hard is headless asset loading?** `AssetSystem::Initialize()` is the very
first thing `Application::Initialize()` does (`Application.cpp:156`), *before* the
window — so asset discovery is already ahead of presentation in the bring-up
order. `LevelRuntime::LoadLevel` (`LevelRuntime.cpp:41-55`), however, is **not**
headless-clean: it calls `cache.Clear()` and
`sceneRenderer.InvalidateAssetBindings()` (48-51) and takes a
`Render::AssetCache&` + `Runtime::SceneRenderer&` — both Render types. The server
needs a **render-free level-load path**: deserialize (`SceneSerializer::LoadFromFile`,
which is in Runtime but only touches ECS + JSON) + `physics.RebuildSceneBodies` +
*skip* the cache/renderer rebind. This is a small refactor — split
`LoadLevel` into a "deserialize + physics" core and a "render rebind" wrapper —
and it belongs in **Stage N2** (headless prerequisite), not left implicit. The
doc's Stage N2 says the sandbox `--server` "loads a level" but does not flag that
`LoadLevel`'s current signature is render-coupled; **this is a concrete N2 task
the plan is missing.**

---

### A5. Reflection codegen & the headless-server link DAG (the biggest structural finding)

**How `*-Generated` registration works.** `assisi_reflect(TARGET, HEADERS...)`
(`cmake/AssisiReflect.cmake:38-109`) runs `reflectgen.py` per annotated header to
emit a `.generated.cpp` of static registrations, compiled into a **separate
OBJECT library** named `${TARGET}-Generated` (line 79-80). OBJECT libraries are
used deliberately so the linker cannot strip the static-initializer TUs the way
it strips unreferenced static-lib members (comment 76-78). Crucially, that object
lib `target_link_libraries(PRIVATE ${TARGET})` (line 85) — **each generated lib
depends on its own module.** Then `assisi_link_reflections(target)`
(113-118) gathers a **global list** (`ASSISI_REFLECT_OBJECT_TARGETS`, appended at
108) and force-links **every** registered object lib into the final exe via
`$<TARGET_OBJECTS:...>`.

**Where reflected components live** (grep of `assisi_reflect` callers):
`modules/Geometry`, `modules/ECS`, `modules/Physics`, and **`modules/Runtime`**
each register. The reflected component set includes:

- `ECS`: `Transform` (`ECS/Transform.hpp`) — render-free. ✅
- `Physics`: `RigidBodyDescriptor` etc. — Core+ECS+Jolt, render-free. ✅
- `Runtime`: `MeshRenderer`, `Camera` (`Runtime/Components.hpp:53,71`), light
  components (`Runtime/LightComponents.hpp`), `NameComponent`, `Hierarchy`,
  `Lifecycle`. **Runtime links Render** (`modules/Runtime/CMakeLists.txt`:
  `Assisi::Render`), and Render links `nvrhi_vk`/`nvrhi`/Vulkan/Window
  (`modules/Render/CMakeLists.txt`).

**The structural problem.** `assisi_link_reflections` is **all-or-nothing**: it
force-links *every* generated object lib. `Runtime-Generated` links
`Assisi-Runtime` (via the PRIVATE dep at `AssisiReflect.cmake:85`), which links
`Assisi::Render` → nvrhi/Vulkan. Therefore **any exe that calls
`assisi_link_reflections` today pulls Render into its link** — and every exe needs
it, because the component metadata (the thing NetSync's codec walks) is *in* those
generated libs. A "`Assisi::NetSync`-using dedicated-server exe" would, with the
CMake as written, transitively link:

```
server.exe
 └─ assisi_link_reflections → Runtime-Generated → Assisi-Runtime → Assisi-Render → nvrhi_vk, nvrhi, (Vulkan), Assisi-Window (GLFW)
 └─ Assisi::NetSync → Core + ECS + Net
 └─ Assisi::App (glue) → Core, Window, Render, Debug, ECS, Runtime, Physics
```

So the plan's clean two-module story ("NetSync mirrors Physics — Core+ECS,
render-free") is true **for NetSync's own object code**, but **false for the
server binary as a whole**, because (1) `App` (the glue layer the doc assigns the
loop integration to) links Render/Window/Debug unconditionally, and (2) the
replicated *component definitions* live in the Render-linked Runtime module and
are dragged in by the global reflection link.

**Does the module DAG allow a headless exe today?** *Link-wise*, only if you
accept linking Render/nvrhi/GLFW into the server (never initializing them).
*Run-wise*, that hinges on whether nvrhi/the Vulkan loader and GLFW are
**lazily** resolved (dlopen/volk at first-use) versus hard-required at process
load. The Render CMake shows NVML is `dlopen`'d at runtime (comment,
`modules/Render/CMakeLists.txt:62`) and nvrhi's Vulkan backend conventionally uses
`VK_NO_PROTOTYPES` + a loader — which *suggests* a device is only touched when
`RenderSystem::Initialize` runs — but **this is unverified on the actual build,
and the doc itself notes the Windows side has never been built at all** (Stage 0
DoD). GLFW similarly may or may not require a display connection merely to be
linked/loaded (it typically only needs one at `glfwInit`, which headless skips).

**Two viable resolutions (the plan should pick one and name it):**

1. **Accept the Render/Window link, forbid only the *init*.** Simplest: the
   server links everything App links but never calls `InitializePresentation()`.
   Cost: fat server binary carrying nvrhi/Vulkan/GLFW; a hard dependency on the
   Vulkan loader / X11 client libs being *present* (not used) on the host —
   which for a containerized Linux server means shipping those .so's in the image
   even though no GPU exists. Must be empirically verified that
   `dlopen`-lazy loading holds (a `--server` smoke test on a machine with **no**
   `libvulkan`/GPU is the acceptance test — and it belongs in Stage N2's DoD,
   which currently only says "ticks physics, logs").

2. **Move replicable game components down to a render-free module + make
   reflection linking selective.** The cleaner long-term fix: relocate
   `MeshRenderer`/`Camera`/`NameComponent`/`Hierarchy`/`Lifecycle`/light
   *component structs* (pure data + `AssetId`s) out of the Render-linked Runtime
   into ECS or a new Core-level `GameComponents` module, leaving only genuinely
   render-owning types (render caches, `SceneRenderer` bindings) in Runtime.
   Then a per-target reflection selection (`assisi_link_reflections(target
   SUBSET ...)` instead of the current global sweep) lets the server link only
   render-free generated libs. This is more work but it is the only path to a
   *truly* Render-free server binary, and it aligns with the doc's own principle
   ("Neither module may depend on Runtime"). Note the doc already anticipates one
   such relocation (`ScopedRawEntityContext` out of Runtime); the component
   relocation is the same shape, larger.

**This is the single biggest gap between the plan's stated architecture and the
CMake reality**, and it should be an explicit decision in Stage N2 (or a new
"N2.5 — headless link DAG"). The doc's assertion that the module DAG cleanly
allows a headless server is **aspirational, not yet true**: `App` and the global
reflection link both currently poison it.

---

## PART B — Operational & Integration Concerns (web-cross-checked)

### B6. Frame-loop integration patterns and GNS threading

The canonical loop structure is Glenn Fiedler's *Fix Your Timestep!* — still "the
gold standard" for a fixed-sim/variable-render loop, whose core (accumulate real
time, run fixed sim steps, **interpolate between two timestamped world states**
for rendering) maps one-to-one onto snapshot interpolation: the same math that
blends physics sub-steps locally blends *snapshot pairs* remotely
([gafferongames.com/post/fix_your_timestep](https://gafferongames.com/post/fix_your_timestep/)).
Assisi already implements this exactly (accumulator + `_interpolationAlpha`,
`Application.cpp:323-334`; `PhysicsWorld::InterpolateTransforms`), so the netcode
plan is not introducing a new loop shape — it is **stamping the existing fixed
step with a `simTick` and hanging snapshots off it** (Stage N3), which is the
minimal, correct integration.

**Where to poll/send.** The engine-standard placement — poll the transport at the
**start of the tick** (drain inbound → apply to sim inputs), step the sim, then
**send after the sim tick** (snapshot/ack out) — fits Assisi's loop with two
insertion points: `Poll()` just before the fixed-step block (or the `DrainMain`
safe point at `Application.cpp:340`), and `Send()` at the end of the
`OnFixedUpdate` body (server) or after `OnUpdate` (client). A dedicated
sim-input-in-fixed-step treatment (sampling input once per fixed tick, not per
render frame — exactly Stage N3's "sampled once per fixed tick from ActionMap
into a ring buffer") is corroborated as the correct discipline by Jakub Tomšů's
*Reliable fixed timestep & inputs*
([jakubtomsu.github.io/posts/input_in_fixed_timestep](https://jakubtomsu.github.io/posts/input_in_fixed_timestep/)):
input consumed by the sim must be latched at fixed-step boundaries or you get
missed/duplicated inputs under variable frame rate. Assisi's current code samples
input **per render frame** (`_input->Poll()` at `Application.cpp:321`, once per
loop iteration, i.e. once per *render* frame not per *fixed* step) — so N3's
"sample once per fixed tick" is a genuine behavioral change, correctly flagged by
the doc.

**GNS thread-safety expectations.** GNS is a **poll-driven** API from the app's
side, with an internal service thread doing the wire work. The app calls
`RunCallbacks()` to dispatch connection-state changes and
`ReceiveMessagesOnConnection`/`ReceiveMessagesOnPollGroup` to drain inbound
([Steamworks ISteamNetworkingSockets docs](https://partner.steamgames.com/doc/api/ISteamnetworkingSockets)).
For scale, **poll groups** are the idiom — one drain call for many connections
rather than per-connection polling — which matters directly for a 2–32-player
server (§B7): the doc's `NetTransport::Poll` should drain a poll group, not loop
connections. A known caveat surfaced repeatedly in GNS issues: internal
thread-safety is via **coarse locking**, and lock contention shows up as a perf
warning / degradation under load
([GNS issue #50 "Excessive lock contention"](https://github.com/ValveSoftware/GameNetworkingSockets/issues/50),
[#307](https://github.com/ValveSoftware/GameNetworkingSockets/issues/307)). At
indie player counts this is a non-issue; it is a note for the "if Poll ever
matters, move it" seam the doc already reserves. The takeaway: **poll on the main
thread once per tick, use a poll group, do not spread GNS calls across threads** —
which is exactly the doc's Stage 1 "threading: none, initially."

### B7. Dedicated server ops

**Can Assisi init headless today?** No — `Initialize()` unconditionally creates a
window and a Vulkan device (`Application.cpp:180-214`). That is precisely what
Stage N2 exists to fix (A1). The *link-time* question (does the server binary
still drag Vulkan/GLFW in?) is answered in A5 and is unresolved. So the honest
status: **nothing headless works today; N2 makes the loop headless; the link DAG
needs a separate decision.**

**Tick loop without vsync.** A headless server must pace itself — there is no
FIFO present to block on. Assisi already has the right primitive: `SleepUntil`
(`Application.cpp:259-282`) is a self-tuning sleep-then-spin that tracks observed
scheduler overshoot (converging to ~60 µs margin on Linux hi-res timers, ~1 ms on
Windows even with `timeBeginPeriod(1)` — the Windows timer-resolution scope is at
`Application.cpp:112-118,293-294`). For a 60 Hz server that is a 16.67 ms budget
with sub-millisecond pacing jitter — more than adequate. This is a real asset:
many engines get server tick pacing wrong (naive `sleep(16ms)` overshoots
badly); Assisi's existing pacing code is directly reusable, repointed from the
render deadline to the next fixed-tick deadline (the doc's N2 says exactly this).
Note the Windows caveat: without `timeBeginPeriod(1)` the sleep granularity is
~15 ms and a 60 Hz loop is impossible — the `TimerResolutionScope` must therefore
be active on the **server** path too (it currently lives inside `Run()`, so a
headless `Run()` keeps it — good, but worth an explicit test).

**Container norms & budgets.** Dedicated-server deployment norms (containerized,
one process per match/instance, no GPU, minimal base image) are standard; the
constraint Assisi must meet is *not shipping a GPU dependency in the server
image*, which loops back to the A5 link decision — resolution (2) (render-free
component module) is what makes a slim `FROM debian-slim` server image possible;
resolution (1) forces bundling `libvulkan`+`libGLFW`+X11 client libs into the
image even though unused. At **2–32 players** with 60 Hz snapshots, per-player
bandwidth is dominated by snapshot size × tick rate; delta replication off change
ticks (Stage 5) is what keeps this bounded — the whole reason the A2 change-tick
fix matters operationally is that *without* it, every replicated entity ships
every tick (full snapshots), and a 32-player server's uplink scales with
entity-count × player-count × 60 Hz instead of *changed*-entity-count. CPU at
those counts is trivial for the sim; the cost centers are codec
serialization and GNS's per-connection locking (B6) — both main-thread, both
inside the 16.67 ms budget with wide margin at indie scale.

### B8. Security / robustness

The universal rule — **treat every client as untrusted; validate all inbound
bytes** — applies squarely to Stage 4's `ByteReader`
([DEV: Cybersecurity for Game Developers](https://dev.to/guardingpearsoftware/cybersecurity-for-game-developers-how-to-protect-your-projects-from-cyber-threats-1cb5)).
Concrete implications for Assisi's plan:

- **Codec fuzzing is not optional, and the plan already gestures at it** ("fuzz
  the reader against truncated buffers," Stage 4). This should be **elevated to a
  first-class, always-run test**, not a nice-to-have: a reflection-driven
  `ReadComponent` walking attacker-controlled bytes is the single highest-risk
  surface in the whole system (a bad length prefix → OOB read/write). The
  `ByteReader` must be bounds-checked on **every** field read (the doc says
  "bounds-checked" — verify this is enforced per-read, not per-message), and a
  libFuzzer/AFL harness over `ReadComponent(meta, bytes)` for every reflected
  component should be a committed CI fixture. General-purpose fuzzing struggles
  with *encrypted* payloads
  ([arXiv 2509.13740](https://arxiv.org/pdf/2509.13740)), so the fuzzer must run
  against the **plaintext codec layer** (post-GNS-decrypt), which is naturally
  where `ByteReader` sits anyway — GNS hands you decrypted bytes.
- **Rate limiting.** The server must cap per-connection inbound message rate and
  reject oversize/overfrequent packets before they reach the codec
  ([DEV, above]). GNS gives some of this (connection-level flow control), but
  application-level command-rate caps (e.g. reject > N `InputCommand`s/sec from
  one connection) belong in `ReplicationServer`. **Add to Stage 5 or a hardening
  stage.**
- **Encryption necessity.** GNS encrypts by default (self-signed certs), so
  Assisi gets AES-over-25519 for free — the doc's Stage 0 crypto-backend work is
  what buys this. For an indie title, GNS's default encryption is sufficient; no
  additional TLS layer is warranted (the web guidance's "HTTPS/TLS everywhere" is
  aimed at web/API traffic, not a UDP game transport that already encrypts). The
  doc correctly defers *identity/auth* (self-signed certs fine until
  matchmaking).
- **Protocol-version negotiation vs the planned protocol hash.** The doc's Stage
  4 protocol hash (hash sorted component names + per-field `(name,type,size)`;
  exchange at handshake; mismatch → reject) is a **stronger** guarantee than a
  hand-maintained version integer: it makes "same component layout on both ends"
  *checked* rather than asserted, and it cannot drift out of sync with the code
  the way a manually bumped version can. The one refinement worth adding: include
  a **coarse human-readable protocol/build version alongside the hash** in the
  handshake, so a mismatch produces a useful "server is build X, you are build Y"
  message instead of an opaque hash mismatch — pure UX, no security cost. The
  hash stays the authority; the version string is diagnostics.

### B9. Testing strategy for netcode

**GNS has built-in network-condition simulation — verified.** Fake lag and loss
are set via global config values `ConfigFakePacketLagSend`/`LagRecv` and
`ConfigFakePacketLossSend`/`LossRecv` (e.g. 20 ms lag + ~10 % loss)
([GNS Go bindings docs](https://pkg.go.dev/github.com/angelskieglazki/gns),
[Steamworks docs](https://partner.steamgames.com/doc/api/ISteamnetworkingSockets)).
Important nuance the test plan must account for: **`CreateSocketPair`'s default
in-process loopback does *not* apply fake lag/loss** — you must pass
`bUseNetworkLoopback = true` to route through 127.0.0.1 ephemeral ports, and
*then* fake lag/loss apply
([Steamworks CreateSocketPair docs](https://partner.steamgames.com/doc/api/ISteamnetworkingSockets)).
So the listen-server zero-latency path (Stage 6) uses the *default* socket pair
(no simulation), while **latency/jitter/loss soak tests use the network-loopback
variant with fake-lag config** — two distinct fixtures from the same primitive.
This is directly actionable and the doc's Stage 1 `CreateLoopbackPair` should
expose the `bUseNetworkLoopback` toggle (it currently sketches only the
zero-latency form).

**Concrete testing stages the plan should add explicitly:**

1. **Codec round-trip + fuzz (Stage 4 DoD, make it teeth).** Round-trip every
   reflected component through `ByteWriter`/`ByteReader`; property-test that
   `read(write(c)) == c`. Fuzz `ByteReader` against truncated/corrupt/oversize
   buffers (B8). This is pure unit-level, no transport — cheapest, highest-value.
2. **Loopback socket-pair as a CI fixture (Stage 1/5).** The `CreateSocketPair`
   path is a real GNS connection with zero real sockets — perfect for CI: spin
   server+client in one process, run a full connect→snapshot→converge cycle
   deterministically enough to assert on. The Stage 0 DoD already uses this for
   the echo test; extend it to a full replication smoke test at Stage 5. **This
   is the backbone CI test for the whole system** and does not need a network.
3. **Latency/loss soak (network-loopback + fake lag).** Run the loopback pair
   with `bUseNetworkLoopback=true` + fake-lag/loss config; assert the client
   converges and interpolation stays smooth under 100 ms lag + 10 % loss over N
   thousand ticks. Catches interpolation-buffer sizing bugs and
   ack/delta-baseline bugs that zero-latency tests hide.
4. **Determinism-free assertion strategy.** Because Assisi is deliberately
   **non-deterministic** (`-ffast-math`, AVX2+FMA, no Jolt
   `CROSS_PLATFORM_DETERMINISTIC` — the doc's own Decisions §), byte-exact
   client==server state comparison is impossible and must not be the test oracle.
   The sound oracle for state replication is **convergence within tolerance**:
   after a snapshot is applied and interpolation settles, assert client entity
   transforms are within ε of the server's *replicated* values (not
   independently re-simulated) — i.e. test the *transport of state*, not
   *reproduction of simulation*. This is the correct testing model for the
   Quake/Overwatch lineage the doc chose and it sidesteps determinism entirely.
   (Determinism-based lockstep testing would need the opposite — and the doc
   already ruled lockstep out.)

The plan mentions testing only inside Stage DoDs. Given that the highest-risk
surface (untrusted codec) and the primary CI mechanism (loopback pair) are both
testing concerns, **testing infrastructure deserves to be a named stage or a
cross-cutting requirement**, not scattered across DoDs (see verdicts).

---

## Implications for Assisi

**Verdict on N2 (headless split design).** *The loop/lifecycle split is correct
and small; the link DAG is the unsolved part.* Splitting `Initialize()` into
`InitializeCore()` + `InitializePresentation()` and replacing the
`_window->ShouldClose()` termination with a `_closeRequested` flag is a faithful,
low-risk refactor — the fixed-step accumulator is already
presentation-independent (`Application.cpp:324-334`), and `SleepUntil` already
paces without vsync. **In-place flag-on-`Application` beats a separate headless
class** (shared sim hooks, shared `SystemRegistry`, and the listen-server
one-scene embedding all favor reuse). But N2 as written omits three concrete
tasks that are prerequisites, not details: (1) `SystemContext`'s hard `InputContext&`/`ActionMap&`
fields (`SystemRegistry.hpp:48-55`) and `SystemRegistry`'s Window link must be
abstracted so headless systems consume `InputCommand`, not a live input device;
(2) `LevelRuntime::LoadLevel` is render-coupled (`cache.Clear()` /
`InvalidateAssetBindings`, `LevelRuntime.cpp:48-51`) and needs a render-free
core split out; (3) **the headless link DAG** — `App` links Render/Window/Debug
unconditionally and `assisi_link_reflections` force-links Runtime-Generated (→
Render), so a server binary drags in nvrhi/Vulkan/GLFW today. Add a decision:
either accept the fat link and *prove* dlopen-lazy loading lets a GPU-less
container run it (make "boots with no libvulkan present" an N2 DoD), or move
replicable component structs out of Render-linked Runtime into a render-free
module and make reflection linking selective. **Recommend a new "N2.5 — headless
link DAG" or fold these three into N2's scope explicitly.**

**Verdict on N3 (tick model).** *Correct and well-supported by the code.* Physics
already steps at a fixed 60 Hz inside `OnFixedUpdate`
(`Application.cpp:324-328`, `PhysicsWorld::Update`, `AppConfig::physicsHz=60`), so
a `simTick++` per fixed iteration is a two-line addition to an already-correct
loop, and it is the exact clock snapshots and inputs should share (Fiedler +
Tomšů corroborate fixed-step input latching). The real behavioral change N3
forces — sampling player input **once per fixed tick** instead of once per render
frame (today's `_input->Poll()` is per-frame) and routing it as `InputCommand`
through `SystemContext` — is correctly identified, but it is entangled with the
N2 `SystemContext` abstraction above; **N2 and N3 share the input-plumbing work
and should be sequenced together.**

**Verdict on the Stage-5 change-tick design.** *Adopt option (a) — a `QueryMut`
yielding `Mut<T>` write-proxies — and downgrade the perceived risk.* The audit's
key correction to the plan: change detection is **already a complete, tested
system** (`SparseSet` per-entity tick lane + `Scene::GetMut`/`Add`/`MarkChanged`
stamping + `Changed(since)` predicate + `TestChangeDetection.cpp`). The "landmine"
is exactly one hole — `Query::operator*` hands out raw `T&` and never stamps
(`Query.hpp:62-65`). The cheapest *sound* fix is a parallel stamping query
(`QueryMut<Ts...>`) whose proxy stamps the pool on mutable deref (Bevy's model),
leaving the existing `Query` untouched for reads and untracked writes; only
player/replicated-write systems migrate. This stamps on *actual writes* (preserving
delta compression, unlike a blanket per-iteration stamp) and converts silent
desync into a wrong-type compile error. Estimate **1–2 days / ~150–250 LOC**,
cheap precisely because the stamping substrate already exists. Reject option (c)
(GetMut discipline) — it relies on human vigilance against the most ergonomic API
in the engine. This should be done **before** any replication code, as the doc
says, but it is a bounded ECS task, not a research risk.

**Missing stages the plan should add:**

- **Testing infrastructure as a first-class concern (not scattered DoDs).** The
  loopback socket-pair (`CreateSocketPair`, default = zero-latency for the listen
  server, `bUseNetworkLoopback=true` + fake-lag config for soak tests) is the CI
  backbone and should be built as a reusable fixture at Stage 1, extended at
  Stage 5. The codec round-trip + **fuzz** harness (Stage 4) is the single
  highest-value safety test and must be committed, not aspirational. The oracle
  must be **convergence-within-ε**, never byte-exact state equality (Assisi is
  deliberately non-deterministic). Recommend a cross-cutting "testing" thread or
  an explicit DoD-teeth upgrade in Stages 1/4/5.
- **Security hardening as a named stage (currently only hinted).** Per-connection
  **rate limiting** on inbound commands (reject flood before the codec runs),
  per-field bounds checks in `ByteReader` (verify enforced per-read), and a
  human-readable build/protocol version string alongside the Stage-4 protocol
  hash (diagnostics, not security). GNS's default encryption covers the wire —
  no extra TLS needed for an indie title. Recommend folding into Stage 5's
  server work or a short "N5.5 — untrusted-input hardening" stage.
- **A headless-link decision (N2.5),** per the N2 verdict above — the plan's
  clean two-module story is true for NetSync's own code but not for the server
  *binary*, and this must be resolved before Stage 5 ships a server anyone runs
  in a container.

**Net assessment.** The plan is architecturally sound and unusually well-matched
to the existing code: the fixed-step loop, the pimpl-over-Jolt physics pattern
NetSync is told to imitate, the transient/descriptor component split, the
change-tick substrate, and the reflection-driven `FieldMeta` codec source are all
**already present and, in the risky cases, already tested**. The gaps are not in
the vision but in three under-specified seams — the headless link DAG, the
`SystemContext` input coupling, and the render-coupled `LoadLevel` — plus a plan
structure that treats testing and input-hardening as DoD footnotes rather than
the load-bearing concerns they are for untrusted-network code. Close those and
N0–N6 is a low-risk, incrementally committable path.
