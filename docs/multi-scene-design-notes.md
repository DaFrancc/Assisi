# Multi-Scene Design Notes

Branch: `multi-scene` (off `dev`, merges back to `dev`).
Status: **S1 and S2 built** (see §4); S3-S5 designed, not started.

Two features any real game needs, neither of which the engine can express
today:

1. **Multiple levels resident at once** — different entities (and eventually
   different network clients) live in different levels, all simulating in one
   process.
2. **In-play level transition ("travel")** — loading into a new level during
   Play without dropping back to edit mode. Today, loading a level forcibly
   sets `_playState = Editing` (EditorLevels.cpp), so a game cannot change
   levels at all while running.

## 0. What exists, verified

- **`ECS::SceneRegistry`** (ECS/SceneRegistry.hpp): named `Scene` store with an
  active pointer. The editor already runs on it — but only ever creates
  `"Main"` and caches `_scene = *mainScene` forever. The primitive for
  multi-scene exists; nothing above it uses more than one.
- **`Physics::PhysicsWorld` already supports sibling instances by design**:
  Jolt globals are refcounted (`AcquireJoltGlobals`, atomic, explicitly
  documented as protecting sibling worlds). One world per scene is
  construction, not surgery.
- **`Runtime::SceneRenderer::Render(frame, scene, camera, ...)` takes the
  scene per call** — it is not bound to a scene. Rendering a different (or a
  second) scene is a call-site decision.
- **`Render::AssetCache` is GUID-keyed and process-wide** — scenes hold
  `AssetId`s; GPU resources are shared. Two scenes referencing the same mesh
  cost one upload. **But the load path clears it**: `App::LoadLevel` calls
  `cache.Clear()` + `InvalidateAssetBindings()` on every load, which frees
  every GPU resource *other* resident worlds' resolved pointers reference.
  Note the Clear also sits in the *wrong place* for travel: it runs during
  the load, mid-transition, while the outgoing world still exists and
  renders. **Cache lifetime policy (v1) — the Clear moves, it doesn't
  stay**: `WorldManager::LoadLevel` never clears during a load. When a
  travel completes and exactly one world remains, Clear + re-resolve the
  survivor — a game client's GPU memory is always one level, and the
  re-upload of assets shared between the two levels lands at the swap, i.e.
  the loading-screen moment. Precisely: the sweep fires when the completed
  travel leaves **one live world plus at most the dormant edited world**
  (editor Play keeps the edited world resident forever, so "exactly one
  world" would never hold there); the survivor re-resolves, and the dormant
  edited world's resolved pointers are **nulled** — it is not rendered, Stop's
  existing `RebindSceneAssetsAndPhysics` rebuilds them, and inspect panels
  null-guard (they must anyway). While multiple *live* worlds are resident,
  no Clear — dangling a live world's pointers is never acceptable. Two
  honesty notes: (1) `Clear()` cancels in-flight loads (safe by construction
  — epoch bump) and waits for GPU idle, and re-resolving the survivor
  **re-imports the whole level from disk asynchronously** — expect
  placeholder pop-in while streams land, exactly like a normal level load;
  "only shared assets re-upload" is what the §5 refcounted upgrade buys, not
  v1. (2) The sweep and `WorldManager::LoadLevel` itself run only at the
  frame's pre-update safe point, marshalled when UI-initiated (the existing
  deferred-load pattern) — Clear must never run inside BeginFrame/EndFrame.
  Open Level keeps today's clear-then-load (single edited world). Headless
  servers use `LoadLevelSim` and have no cache at all.
- **Entity handles are scene-local** (index/generation into that scene's
  registry). Cross-scene `EntityRef`s are meaningless — moving an entity
  between scenes must be serialize-out/recreate, and the reflection layer
  (ComponentMeta serialize/deserialize + the level serializer's EntityRef
  resolution) already provides the machinery.
- **The single-scene assumptions live above ECS**: one `PhysicsWorld` member
  in `EditorApp`, one `_scene` pointer feeding every panel, undo history bound
  to that scene, the play snapshot/restore machinery bound to it,
  `LoadLevel(scene&, ...)` loading *into* a caller-owned scene, and
  `Application`'s frame loop assuming "the" scene.

So this is not a rewrite. It is promoting an existing seam (SceneRegistry) to
the unit the app layer actually thinks in.

## 1. The core abstraction: `App::World`

A level at runtime is not just a `Scene` — it is a bundle:

```
World {
    ECS::Scene           scene;
    Physics::PhysicsWorld physics;      // per-world; Jolt globals refcount
    std::string          name;          // unique key; NOT the level path (travel A→B→A
                                        //   needs two worlds of one path — generated names)
    std::string          levelPath;     // virtual path, empty = none
    WorldState           state;         // Loading | Active | Dormant | Unloading
    bool                 simulate;      // stepped by the fixed loop?
    bool                 streamingPending; // UpgradeStreamingAssets' per-scene flag
    std::uint64_t        propagationTick;  // transform-propagation bookmark (see below)
    // future (networking branch): std::unique_ptr<NetSync::NetSession>
}
```

Two structural requirements, stated so they are contracts rather than luck:
- **World (and Scene) addresses are stable for a world's lifetime** —
  `EditHistory`, panels, and (later) `NetSession` hold references. The store
  must be node-stable (unique_ptr values, as SceneRegistry already does).
- **The active world cannot be destroyed** — `WorldManager::Destroy` refuses
  unless a successor is named (or it is the last world, which is shutdown).

`App::WorldManager` owns them (name → World, like SceneRegistry but at the
bundle level; SceneRegistry either retires into it or stays as its internal
store). Rules:

- The **fixed-step loop iterates every world with `simulate == true`**;
  per-world physics steps that world's scene only. Two levels never collide,
  because they are two Jolt worlds. Whether a world with no players in it
  ticks is **game policy, not engine policy**: the engine exposes the
  per-world `simulate` flag and a configurable default; the game decides
  (there is no player concept in the engine yet, so v1 default is simply
  "resident worlds tick unless the game turns them off").
- **One world is *rendered*** per window/viewport: the WorldManager's active
  world feeds SceneRenderer. Dormant worlds simulate (or not) without
  rendering. Split-screen/multi-viewport later = more Render calls, no design
  change. **The renderer is not scene-stateless though**: its
  transform-propagation bookmark (`_lastPropagationTick`) is per-renderer and
  compared against the per-scene change tick — switching worlds through one
  renderer would skip propagation (or redo all of it). The bookmark moves
  into the World bundle. And unrendered simulated worlds need **two** things
  the render path currently provides, not one: Jolt poses reach `Transform`
  components only via `PhysicsWorld::InterpolateTransforms`, which runs in
  the render path for the rendered scene — propagation alone would produce
  up-to-date matrices of *stale positions*. So the sim loop gives
  simulated-but-unrendered worlds a **pose write-back** (post-step
  `InterpolateTransforms`/`SyncTransforms` into their scene) *and then*
  `PropagateTransforms` — otherwise their transforms sit at spawn pose
  forever, and replication of dormant-world state would ship spawn poses.
- **Per-world physics shares one Jolt job system.** Each `PhysicsWorld`
  today constructs its own 10 MiB temp allocator and a thread pool sized
  `hardware_concurrency()-1`; N worlds would mean N thread pools
  (oversubscription) — against the performance-first rule. Jolt supports a
  shared `JobSystem` across `PhysicsSystem`s (pool and temp allocator are
  per-`Update`-call arguments, so this is construction-level work): S1
  hoists them to a shared service. Ordering caveats the service owns: Jolt's
  globals (allocator/Factory/RegisterTypes) must be acquired before *any*
  Jolt allocation including the pool's construction — the service does its
  own Acquire/Release of the existing refcounted globals and is declared to
  outlive every World. Sharing one temp allocator is valid only because
  worlds step **sequentially** in the fixed loop; parallel world stepping
  would need per-world allocators (noted, not planned).
- **`SystemContext` carries the world** (scene + physics + dt + ...), so game
  systems are world-agnostic — the same system runs in whichever worlds it is
  registered for. **World-affinity rules** (settled during S1, enforced by
  convention + review):
  - A system is either *stateless* (its state lives in components) or
    *instantiated per world*. Never a shared instance with cross-frame state
    running over several worlds — statics/accumulators advance N× too fast.
  - Cross-frame caches keyed by `Entity` must not span worlds: handles are
    scene-local, so world A and world B both contain `(index 0, gen 1)` — the
    same index into two different arrays. Same-world code keeps plain
    `Entity` (the overwhelming majority; zero overhead added). The rare
    cross-world reference uses a **`WorldEntity { world, entity }`** pair, or
    the cache lives inside the world bundle. This applies to editor state too
    (`_selectedEntity` belongs to a specific world).
  - **Input-consuming systems run only in the active world** — one
    `InputContext`, N worlds; running a controller system everywhere applies
    the same keypresses in every world. The editor fly-camera is a temporary
    stand-in here (no player concept exists yet); it follows the active world
    and gets revisited when a real player/possession concept arrives.
  - **Events follow the same rule as caches**: the app's single `EventQueue`
    is fine for app-level events, but any event carrying an `Entity` payload
    is world-ambiguous once two worlds exist — such events carry
    `WorldEntity` (or move to a per-world queue in the bundle).
- **FlushDestroyed per world**, end of frame, as today.
- **Asset resolve is per scene** and already shaped that way
  (`ResolveSceneAssets(scene, cache, db)`); the streaming upgrade loop runs
  over each resident world.

Editor mapping: `_scene`/`_physics` become "the active world"; every panel
keeps working against the active world's scene exactly as today. The
hierarchy panel gains a **world selector** (a small dropdown listing resident
worlds) so other worlds can be inspected. **The editor commits to exactly one
*edited* world**: Save, dirty tracking, and the undo history bind to it and
it alone; other resident worlds are **inspect-only** (read-only panels, like
mirrors in the networking plan). "Edited world" is a persistent
`WorldManager` role, distinct from "active world" — during Play the active
world may be a travel-created one while the edited world sits dormant, and
Stop retargets restoration at the *edited* world explicitly. Per-world
editing (multiple histories, multiple dirty states) is deliberately out of
scope; nothing in the bundle precludes it later.

## 2. In-play travel (feature 2)

**Model: travel creates a world; Stop destroys everything Play created.**

- `WorldManager::LoadLevel(levelPath)` (the game-facing API — deliberately
  the plain name; "travel" below is only prose for the concept, not API. The
  existing `App::LoadLevel(scene, ...)` free function becomes this method's
  internal step during the S1 refactor, so the name is inherited, not
  colliding):
  1. Create a new world, load the level into it (assets resolve through the
     shared cache; async streaming already upgrades placeholders).
  2. When ready, swap: new world becomes active + simulated, old play world
     becomes `Unloading` and is destroyed after the frame.
  3. Optionally migrate flagged entities (below).
  4. **On failure** (missing file, parse error): destroy the half-created
     world, keep playing in the current one, surface the error (log + Game
     panel). A failed travel must never strand the game between worlds.
  5. **Travel from Paused discards the pause scratch history first** (the
     same reset `ResumePlay` does). The scratch history binds the played
     scene by reference; travel destroys that world, and a stale binding is
     a two-click use-after-free (F6, then travel). Alternatively the debug
     control could disable while Paused, but the reset is the general fix —
     game code can travel from any state.
- **Editor Play/Stop semantics stay exactly as strong as today**: the play
  snapshot (capture at StartPlay, ReviveAt-restore at StopPlay) belongs to the
  *edited* world. Travel never touches the edited world — it replaces the
  world being *played*. Stop destroys every world Play created and restores
  the edited scene from the snapshot at exact identity, so undo history
  survives any number of travels. (First travel away from the edited level:
  the edited world simply goes Dormant + unsimulated; it is restored on Stop
  like today.)
- **The editor's level menu is relabeled "Open Level"** (Unreal's editing
  gesture name) and stays an edit-mode action — "open" = change what I'm
  editing, `LoadLevel` = the game changed level. Open Level **reuses the
  edited world, clearing its scene in place** — it does not create a fresh
  world. This preserves the EditHistory invariant the code already relies on
  (history binds `Scene&`; loads clear the same object, never swap it) and
  keeps the AssetCache Clear correct (§0). During Play, Open Level becomes
  **disabled** — a new, small behavior change: today it is not disabled, it
  silently force-drops the session to Editing, which is worse. Travel is a
  *game* action, exposed for testing via a small "Load level..." debug
  control in the Game panel. This keeps
  "editing which level I'm working on" and "the game changed level" from
  sharing a button.
- **Hard travel first** (blocking load, one hitch frame). Async travel — load
  the new world in the background while the old one keeps simulating, swap on
  ready, loading-screen hook — is a polish stage on the same API, not a
  different design, because a Loading world is just a resident world that
  isn't simulated or rendered yet.

### Entity migration (persistent entities across travel)

`MigrateEntity(srcWorld, dstWorld, entity)`: serialize the entity's
components via ComponentMeta, recreate in the destination, destroy the
source. Subtree-aware (children travel with it); `EntityRef`s within the
migrated set remap, refs to left-behind entities null with a warning. The
game marks what travels (the player, their inventory) — everything else
belongs to the level. This is Unreal's persistent-across-travel set. Honest
scope note: the serializer's two existing EntityRef contexts both fit *whole
scenes* (full save/load remap clears the destination first; the raw-identity
context requires ReviveAt at exact handles, which would collide with the
destination's own allocations). Migration needs a **third mapping mode** — an
explicit src→dst handle map with null-and-warn for out-of-set refs — plus
**per-destination transient rebuilding**: the Jolt body is removed from the
source world's PhysicsWorld and rebuilt from `RigidBodyDescriptor` in the
destination's, and MeshRenderer pointers re-resolve there (the
`ApplyEditRebind` pattern, generalized off the single `_physics`). S4 is
sized accordingly. The EntityRef context is also `thread_local` — S5's async
travel must deserialize on a thread that owns its own context.

## 3. Networking tie-in (coordination with the `networking` branch)

Designed here so the branches converge instead of colliding:

- `NetSession` already binds `Scene&`, so **session-per-world** is the
  natural extension — a server hosting N levels runs N worlds each with its
  own session (simplest: own port each; single-port world routing is a later
  optimization). **One named API change the rebase must make**: NetSession's
  current contract forbids scene swaps (destroy the session, don't rebind)
  and it owns its transport, so "travel without dropping the connection"
  requires splitting **transport lifetime from world binding** — the
  connection outlives the world; the replication client detaches from the
  old world's scene and attaches to the new one (its identity maps clear on
  attach, which the replication plan's §2 map-clear API already provides).
  Without this split, client-side travel would tear down the socket with the
  world — the exact opposite of the feature.
- The replication plan's level handshake (path + hash, tagged addressing)
  extends to travel: server sends a `LevelChange` control message; the client
  re-runs the §2 join sequence (load → strip → map-clear → resync) into a
  fresh world without dropping the connection. Client-side, "which world is
  rendered" follows the session.
- **Merge order — decided**: multi-scene lands in `dev` first (S1 at
  minimum) and `networking` rebases onto it, so the replication work binds
  sessions to worlds from the start rather than to the lone scene and
  refactoring twice.

## 4. Milestones — each with a user-visible definition of done

**S1 — The World refactor (behavior-identical). BUILT.** Introduce `World` +
`WorldManager` (stable addresses, edited-world role); editor and app loop
run on the active world; per-world physics over a **shared Jolt job
system/thread pool**; Open Level clears the edited world in place.
*DoD*: the editor looks and behaves exactly as before (load, edit, play,
undo, save all unchanged); full test suite green; no panel knows the
refactor happened; one thread pool total (verified — not one per world).

*As built*: `App::World`/`App::WorldManager` (modules/App/World.hpp) with
`TestWorld.cpp` covering address stability, creation-order iteration, the
role-holder destroy refusal, and — measured against `/proc/self/task` — that
four extra worlds add zero threads. `ECS::SceneRegistry` retired into the
manager and deleted. The Jolt globals block in PhysicsWorld.cpp became a
refcounted `JoltRuntime` holding the one thread pool and temp allocator,
acquired as `Impl`'s first member so the ordering is right by construction.
`SceneRenderer::Render` gained an overload taking the propagation bookmark,
which now lives in the world; the single-scene overload keeps the renderer's
own. The editor's `_scene`/`_physics` are the active world's, `SetPlayState`
is the one place play state and the world's `simulate` flag move together,
and the fixed loop steps every simulated world.

**S2 — Multiple resident worlds. BUILT.** Create/load/destroy worlds at runtime;
fixed loop steps all simulated worlds; **pose write-back + transform
propagation** for unrendered simulated worlds (§1 — poses first, matrices
second); cache-lifetime policy (load without Clear); hierarchy panel world
selector. Rule: outside Play, non-edited resident worlds are
inspect-only and do not simulate (nothing mutates without a restore story).
*DoD*: in Play, a debug control loads a second level as a second world; both
step physics concurrently (the selector shows bodies in each settling
independently, world matrices correct in both); destroying the second world
leaves the first untouched; Stop destroys every Play-created world and
restores the edited level exactly. Asset check: loading the second world
does not invalidate the first world's meshes/materials (no cache Clear).

*As built*: `LoadLevel` gained an `AssetCacheReset` argument — the second
world loads with `Keep`, so the Clear is no longer unconditional. Physics
poses reach unrendered worlds via `PhysicsWorld::SyncTransforms` (the
no-blend write-back) followed by propagation, packaged as
`App::SyncUnrenderedWorld`; `TestWorld.cpp` pins both the ordering (the
matrix must agree with the *post*-write-back position) and world
independence (a floor in one world does not catch the other's falling body).
The editor drives it from the Game panel's **Load as new world** debug
control, available during Play only — while Editing there is exactly one
world, which keeps Play/Stop's snapshot unambiguous. The Entities panel
gained a world dropdown (hidden until a second world exists), an
`IsEditable()` predicate gates every mutating control, and `StopPlay`
destroys the session's worlds before restoring. Not yet covered by an
automated check: that the second load leaves the first world's GPU assets
intact — that one needs a device, so it is an eyes-on check.

**S3 — In-play hard travel.** `WorldManager::LoadLevel` + Game-panel debug
control; Play/Stop semantics preserved via the edited-world role.
*DoD*: press Play in level A, travel to level B from the debug control, keep
playing in B (physics live, no return to edit mode), travel again, then Stop
→ the editor is back in level A exactly as edited, dirty state truthful,
undo history intact. A travel to a **nonexistent level** mid-play fails
loudly, destroys nothing but the half-created world, and play continues in
the current level. Deferred `Destroy`s queued in the edited scene right
before travel still flush. After a completed travel the cache sweep ran
under its real editor-Play condition — one live world plus the dormant
edited world (verified via the cache's counts in a log line), the edited
world's resolved pointers nulled and rebuilt correctly by Stop — and the
new world renders after it, with streaming pop-in allowed exactly as on a
normal level load. Travel from Paused neither crashes nor leaks a stale
pause history.

**S4 — Entity migration.** `MigrateEntity`, subtree-aware, with the new
src→dst EntityRef mapping mode and cross-world transient rebinding (Jolt
body out of the source PhysicsWorld, rebuilt in the destination's; mesh
re-resolve) — see §2's scope note.
*DoD*: an entity spawned in level A travels to level B with its component
state (position set before travel shows in B); its children arrive with it; a
ref to a left-behind entity nulls with a logged warning, not a crash.

**S5 — Async travel + docs.** Background world load with swap-on-ready and a
loading-screen hook; update this doc's status, remaining-work.md, and memory.
*DoD*: travel from a heavy level shows the old world simulating until the
swap (no multi-second hitch), verified with the frame profiler.

Order: strictly serial. S1 is the load-bearing refactor and must merge to
`dev` early (see §3 merge-order note).

## 5. Explicitly out of scope here

- **Additive/streamed sub-levels** (one world composed of multiple level
  files, Unreal streaming-sublevel style): different feature — composition
  *within* a world vs multiple worlds. The World bundle doesn't preclude it;
  a world's scene could later load several files. Not designed here.
- **Cross-world rendering** (portals, split-screen): the renderer is already
  per-call; wiring more viewports is future work.
- **AssetCache eviction** (refcounted per-world release, so long sessions
  crossing many levels reclaim GPU memory): v1 grows to the union of
  resident-since-last-unload levels, which is bounded and correct; eviction
  is an optimization with a clear seam (the cache already tracks ids).
- **Per-world editing** (multiple undo histories, per-world dirty/save): the
  editor commits to one edited world in v1; the bundle doesn't preclude more.
- **Networked travel and multi-world hosting implementation**: designed in §3
  for shape, implemented on the networking side after both branches merge.
