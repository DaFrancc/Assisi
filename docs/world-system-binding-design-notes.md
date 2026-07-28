# World–System Binding Design Notes

Branch: planned on `multi-scene` (after S1-S5; this is the "per-world
registration question" deferred at EditorApp.cpp:632-633).
Status: **planned, not started.** Plan reviewed by two independent
adversarial passes (one codebase-grounded, one architecture/research);
their accepted findings are folded in below and marked where they changed
the design.

## 0. Problem

Systems bind to "the" scene, not to worlds. `App::SystemRegistry` is owned
by the app — EditorApp holds `_systems` (editor tools) and `_gameSystems`
(game logic) — and every `Run()` call passes a single scene. With
multi-scene built (multiple resident worlds, dormant worlds, async
travel), there is no mechanism for "which systems run in which world."
multi-scene-design-notes.md §1 ("world affinity") settles the *rules* —
SystemContext carries the world; systems are stateless or instantiated per
world; input-consuming systems run only in the active world — but not the
*mechanism*. This document is the mechanism.

## 1. Decision

**Each `World` owns a `SystemRegistry`. Named profile installer functions
— defined in code, selected by name in level data — populate it exactly
once at a well-defined commit point. Per-frame dispatch iterates simulated
worlds and runs each world's own registry; per-frame cost is governed by
resident entities (an empty-pool gate skips idle systems), never by
registration count.**

Why per-world instances and not the alternatives:

- **A single app-owned registry filtered at dispatch** violates §1's
  stateless-or-instantiated-per-world rule for any stateful system —
  cross-frame state in a shared instance advances N× too fast across
  worlds. Per-world registries make the rule structural: state lives in
  the registered lambdas' captures, and each world gets fresh captures.
- **A .cpp file per level** binds systems to the wrong axis: travel
  A→B→A leaves two worlds of one level resident, and in an open-world
  game "level" stops meaning "world" entirely. Worlds are runtime
  instances, not types.
- **Industry survey (verified with sources during review):** Unity DOTS
  is this design almost exactly — each `World` instantiates its own
  system instances, selected by bootstrap code, with `RequireForUpdate`
  as the idle skip. flecs stores systems *in* the world with importable
  modules as the reuse mechanism and skips empty-table systems. Bevy
  looks decoupled (Schedule::run(&world)) but caches system state
  against a specific world inside the schedule — even the shared-schedule
  poster child instantiates per world under the hood. Nothing surveyed
  fits this codebase's constraints (heap-pinned worlds, handful in
  count, capture-based system state) better than per-world registries.

### The four game shapes, and what this layer does and does not cover

The requirement: one mechanism serving (a) many small distinct levels,
(b) one giant open world, (c) sectioned worlds (Arkham-style),
(d) fully-resident aggressively optimized maps (Counter-Strike-style).

The unifying principle is keeping two questions orthogonal: *what
simulates together* (the World — scene + physics + systems) and *what is
resident right now* (a future content/chunk streaming layer). Binding is
per-world and coarse; residency changes only entities, and cost follows
resident awake entities:

- (a) distinct levels: one profile per level *kind* (profiles are
  many-to-one — fifty combat levels share "Gameplay"); travel between
  worlds is the existing S3/S5 machinery.
- (b) open world: one long-lived world, one profile installing the full
  system set; the empty-pool gate makes idle systems cost one compare;
  streamed-out regions' entities leave the pools, so queries shrink.
- (c) Arkham: chunks within one world (systems installed once, data-gated)
  *or* travel through vestibules for sim-isolated interiors — both
  expressible per level; shipping both is industry-standard (Unreal has
  streaming *and* travel), not a smell.
- (d) CS-style: maps are fully resident and fully simulated for the whole
  match; the aggressive part is precomputed visibility (PVS). That is
  render-side culling **plus PVS-driven network replication filtering**
  (Source uses PVS for both) — the first is a renderer concern, the
  second belongs to the networking branch. No residency machinery needed.

**Honest scope limits (from review):**

- Streaming is orthogonal to *registration*, not to the system
  *contract*: a chunk layer will eventually extend
  SystemContext/events/lifecycle (OnChunkLoaded-style hooks, observers) —
  every surveyed engine grew these. And bulk Jolt body insertion into a
  *live stepping* world cannot reuse S5's worker trick (that relies on the
  Loading world's physics being untouched); it must be main-thread
  marshalled/batched. Claiming full orthogonality overstates it.
- The empty-pool gate is not "the open-world enabler." It makes
  *installing* a full system set affordable. The open world's real walls
  are elsewhere and out of scope here: serial single-threaded system
  dispatch, float precision / origin rebasing, Jolt broadphase scale, and
  residency itself.

## 2. Stage P1 — SystemContext carries the world

1. `SystemContext` replaces `ECS::Scene &scene` with `World &world` and
   adds `bool isActiveWorld`. `World` is forward-declared in
   SystemRegistry.hpp (it includes no App headers; App's CMake deps are
   PUBLIC, so no build change — verified). SystemRegistry.cpp includes
   World.hpp.
2. Migration is mechanical: `ctx.scene` → `ctx.world.scene`. The editor's
   own `_systems` stays app-owned — picking/camera/selection are
   properties of the editor viewing a world, not of any world — and is
   dispatched against the viewed world.
3. `SystemHandle::ActiveWorldOnly()`: dispatch skips the entry when
   `!ctx.isActiveWorld`. This mechanizes §1's "one InputContext, N
   worlds" rule instead of leaving it to convention.
   **Semantics decision:** in the editor, `WorldManager::Active()` means
   "the world being viewed" — the world selector repoints it, including
   to the dormant edited world mid-play. So input follows the view:
   switching the selector mid-play intentionally parks the play world's
   input systems. Document at the declaration. A game build's Active is
   simply the played world.
4. **Headless test seam:** `SystemContext` cannot currently be built
   without a window (`Window::InputContext`'s only ctor takes a live
   `WindowContext`). Make `input`/`actions` pointers in the context (null
   in headless tests) — headless is already a first-class Application
   shape.

## 3. Stage P2 — World owns its registry; WorldManager knows profiles

1. `World` gains `SystemRegistry systems;` and `std::string profile;`
   (worlds are heap-pinned and non-movable, so registry addresses are
   stable).
2. `WorldManager` gains
   `using ProfileInstaller = std::function<void(World &)>;`,
   `RegisterProfile(name, installer)`, `SetDefaultProfile(name)`, and
   private `ApplyProfile(World &, nameOrEmpty)`.
   **Fail loud:** an unknown profile name is an error log + fallback to
   the *default* profile — never warn-plus-empty. A typo'd `"profile"` in
   a level file must not ship a world where physics steps but no game
   logic runs.
3. Profiles compose in code: installer functions
   (`InstallCoreGameplay(world)`) that profiles call, so "level B = level
   A's systems + one more" costs one line. Additive only — no subtractive
   composition (removal breaks After/Before constraints and inverts the
   cost model; something repeatedly subtracted from the base is a feature
   that should not be in the base).
   Installers run **once per world**, not once at startup — one-time side
   effects in installers are forbidden (document it).
4. **Re-application:** `SystemRegistry` gains `Clear()`. The registry is
   append-only today and duplicate names corrupt the constraint graph, so
   the rule is: `Create()` installs nothing; each commit point applies
   exactly once; re-targeting a live world (Open Level reuses the edited
   world) is `Clear()` + apply.
5. **Profile plumbing — commit points, exhaustive.** (Review-critical:
   the naive insertion points — WorldManager::LoadLevel + promote — miss
   every path the editor actually uses for the world you edit and play,
   leaving F5 running only the default profile forever.)
   - `.alvl` gains optional top-level `"profile": "Name"`.
     `SceneSerializer::LoadFromFile` gains a level-header out-param
     (`struct LevelHeader { std::string profile; }`) — it currently
     parses and discards the JSON root. `App::LoadLevel` (LevelRuntime)
     surfaces the header to callers.
   - Commit points: (1) `WorldManager::LoadLevel` sync path — after
     deserialize, before `SwapToActive`; (2) `PromotePendingLoad` — after
     the worker is joined and reports ok (race-free: workers never run
     installers; the worker stashes the header string in `World::profile`
     for the main thread); (3) editor **Open Level**
     (`LoadLevelFromPath`) — `Clear()` + apply on the reused edited
     world; (4) the startup level load (same path); (5) **Load as new
     world** (`LoadLevelAsNewWorld`); (6) bare `Create()` with no
     subsequent load (the editor's initial empty world) — the app applies
     the default profile after creating it.
   - **Save round-trip:** the editor's `SaveLevel` writes
     `World::profile` back into the file (Save takes the header).
     Without this, every editor re-save silently strips the field.
6. `EditorConfig::registerGameSystems` (`void(SystemRegistry&)`) is kept
   and bridged as the default profile's installer:
   `[fn](World &w){ fn(w.systems); }` — no config API break. The default
   profile must be registered with WorldManager *before* the first
   `Create()` (today the initial world is created before the config is
   consumed — reorder). Relocate the `HasRenderSystems()` warning so it
   fires once, not per world.
7. `EditorApp::_gameSystems` is removed; play-mode game systems come from
   the played world's own registry.

## 4. Stage P3 — dispatch per world

- **Uniform simulating gate.** (Review-critical: gating FixedUpdate on
  `state == Active && simulate` alone regresses Pause — Pause clears only
  the *viewed* world's `simulate` flag, and travel-from-pause force-sets
  it on the incoming world, so secondary play worlds would tick game
  logic while the editor says Paused.) In the editor, **all** game phases
  — FixedUpdate included — are additionally gated on `IsSimulating()`.
  And the "simulate follows play state" comment is already false for
  travel-from-pause; stop asserting it. (Alternative — Pause clears every
  play world's flag — rejected: it destroys per-world intent.)
- `OnFixedUpdate`: when `IsSimulating()`, for each world with
  `state == Active && simulate`, run its FixedUpdate phase
  (`isActiveWorld = (&world == worlds.Active())`), then step that world's
  physics — folded into the existing sequential physics `ForEach` so the
  apply-forces-then-simulate ordering holds per world.
- `OnUpdate`: same iteration for PreUpdate/Update/PostUpdate when
  `IsSimulating()`, after the editor's own `_systems`.
- Loading and Dormant worlds are never dispatched. (Verified: no current
  code path dispatches a Loading world; promotion joins the worker before
  anything else, so install-at-promotion cannot race it.)
- Single-world behaviour is equivalent to today (verified). Multi-world
  behaviour is *intentionally not preserved*: today, viewing the dormant
  edited world mid-play runs Update-phase game systems against the
  dormant world's scene while FixedUpdate stops — a live bug that
  per-world dispatch fixes. Owned as a change, not preservation.
- Events, short-term rule (documented, not built): the single app
  `EventQueue` stays; once two worlds can emit, any event carrying an
  `Entity` payload must carry `WorldEntity{world, entity}` instead
  (§1's rule). Per-world queues are a later decision.

## 5. Stage P4 — data-driven activation gate

`SystemHandle::RequireAny<Ts...>()` (plus a runtime-ComponentId form):
dispatch skips the system when every listed component pool in
`ctx.world.scene` is empty. This is Unity's `RequireForUpdate` / flecs's
empty-table skip, hand-rolled, and it is what lets an open-world profile
install everything.

- ECS work: no pool-size query exists today (`Scene::GetPool<T>` is
  private; the type-erased `PoolStorage` has no size accessor), but pools
  are ComponentId-indexed arrays and `SparseSet::Size()` exists — a
  ~10-line addition. A never-created pool reads as size 0. Deferred
  `Destroy()` means the gate can over-run by one frame for
  already-queued entities — harmless.
- Idle cost (verified against the actual pool layout): one or two indexed
  loads + compares per idle system per phase; hundreds of systems remain
  well under 0.1 ms/frame. Registration count does not appear in the
  frame-cost equation; resident awake entities do.
- Known fat: each registry `Entry` carries a `std::string`, a
  `std::function`, and two `vector<string>`s, so the walk is a scattered
  cache tour and running systems pay `std::function` dispatch. Noise at
  realistic counts; if system counts genuinely reach hundreds, compact to
  a packed dispatch array *then*, not now.

## 6. Deferred / future work (recorded here so it isn't re-litigated)

- **Runtime enable/disable + `RequireNone`** — the *first* post-P4
  additions. An in-world pause menu or cutscene needs to switch gameplay
  systems off; every surveyed engine has this; creation-time profiles +
  ActiveWorldOnly + the empty-pool gate cannot express it.
- **Profile coverage audit (debug tooling):** components present in a
  scene vs. the union of installed systems' RequireAny declarations —
  catches "water authored/streamed into a world whose profile installed
  no WaterSystem" at load time instead of in QA. Without it that failure
  is a silent no-op, undiagnosable because systems are opaque
  `std::function`s.
- **Chunk/content streaming layer** (separate design doc when started):
  chunk ids on entities, additive loads into a live world, unload of a
  chunk's entity set, residency policies (always / grid / zones) selected
  by level data, soft cross-chunk EntityRefs (resolve on load, null when
  absent — same discipline as §1's cross-world refs). Plus the contract
  growth noted in §1 above (lifecycle hooks, batched live-world Jolt
  inserts, travel cancelling in-flight chunk streams).
- **Travel policy:** `WorldManager::LoadLevel` hardcodes
  destroy-outgoing; revisitable-with-state levels (Metroidvania
  backtracking) need a keep-dormant or serialize-out policy later. A
  travel-policy question, not a binding question.
- **Open-world blockers outside this design:** parallel system dispatch,
  origin rebasing / precision, broadphase scale, residency.

## 7. Testing

- Unit (App/tests, headless via the P1 input seam): two worlds sharing a
  profile hold independent captured state; `ActiveWorldOnly` skips in
  non-active worlds; unknown profile → default + error; default applies
  on empty name; `Clear()` + re-apply (the Open Level path); RequireAny
  gate off→on→off transitions; After/Before ordering unchanged per world;
  the `registerGameSystems` bridge (its sole in-repo caller passes
  nullptr, so nothing exercises it otherwise).
- Editor smoke: single-world play/pause/stop identical to today; Pause
  with a secondary play world resident ticks **no** game logic anywhere;
  travel installs the destination profile; travel-from-pause runs no
  logic until Resume; Open Level re-applies the profile on the edited
  world; the world selector mid-play parks play-world input (documented
  semantics); re-saving a level preserves its `"profile"` field.
