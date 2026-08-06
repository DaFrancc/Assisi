# Blueprints — review round 7 findings

Five parallel reviewers over `dev..HEAD` on branch `blueprints` (50 commits, 141
files, +21,033/−4,111), 2026-08-05. Slices: Runtime, Editor, App+apps,
NetSync+Core/ECS, reflectgen+the five strong-type commits.

**Status: this branch is not mergeable as it stands.**

`[VERIFIED]` = I reproduced or confirmed it directly in this session.
Everything else is a reviewer's reading, unconfirmed by me — check before acting.

---

## 0. The pattern worth naming first

**Four places where a comment or doc asserts behaviour the adjacent code does not
implement.** All four are mine. Each is worse than a plain bug: it tells the next
reader the thing is handled.

1. `[VERIFIED]` `Blueprint.cpp:255-260` — `QualifyName`'s comment explains that a
   leading `/` diverges for an override. There is no override branch. (→ B2)
2. `[VERIFIED]` `docs/blueprint-build-status.md:368-371` claims the Levels-panel
   Save was fixed to use `SaveLevelToPath(_world->levelPath)`. The line is
   byte-identical to `dev`. (→ B1)
3. `ApplySystems` comment claimed `systemNames` was recorded outside the failure
   guard; it was inside. Already fixed in `4905c59`, listed here as evidence of
   the pattern.
4. `[VERIFIED]` `docs/blueprint-build-status.md:82-85` says the `InstanceView`
   storage ban has "no `transient` escape hatch". There is one. (→ B14)

**Sweep every load-bearing comment on this branch against its code before
trusting any of them.**

---

## 1. Corrupts user data — reachable through the UI

### B1 `[VERIFIED]` Levels-panel Save writes to the combo box, not the open level
`modules/Editor/src/EditorLevels.cpp:97` — `SaveLevel(_levelFiles[_selectedLevel])`.
Untouched from `dev`; the documented fix does not exist.
`ScanLevels` forces `_selectedLevel = 0` (`:269`); nothing syncs it to the open
level (startup `EditorApp.cpp:179`, blueprint close, travel, join all bypass it).

*Failure:* boot with `levels/Sandbox.alvl`; `_levelFiles` sorts to
`[Materials, NetPile, Sandbox, Test]`; press Save → **`Materials.alvl` is
overwritten with Sandbox's contents**, `_world->levelPath` retargets to
`levels/Materials.alvl` (`:673`), the dirty `*` clears, and `ReexpandInstancesOf`
runs on the wrong path (`:691`). "Refresh" mid-session reproduces it too.

*Fix:* `SaveLevelToPath(_world->levelPath)`. Two lines. Also fix the doc.

### B2 `[VERIFIED]` An override's `/`-prefixed reference is wrongly qualified → the level stops loading
`modules/Runtime/src/SceneSerializer.cpp:627`, `:711-713`;
`modules/Runtime/src/Blueprint.cpp:261-266` (`QualifyName`).
Concept §6: a leading `/` in an *override* means the placing file, so it must not
take the instance prefix. `CommitInstance` qualifies every path component
including override-sourced ones; `QualifyName` strips the `/` and prefixes anyway.

The editor authors exactly this form: `EditHistory.cpp:332` writes `"/" + name`
for a level entity, `:326` writes `"/" + row->name + "/" + memberPath`.

*Failure:* wire `car_3/body`'s `Parent` to a level entity `spawn_marker` → saved
as `"parent": "/spawn_marker"` → reload qualifies it to `car_3/spawn_marker` →
unresolved → `Load` throws (`SceneSerializer.cpp:1074`) → scene cleared,
`LoadFromFile` returns false. **The level no longer opens**, naming a member
nobody wrote. Silent variant: resolves to the wrong entity if a leaf matches.

*No test anywhere.*

### B3 `[VERIFIED]` `BlueprintMemberDesc::parented` is never recomputed after an override adds/removes `Parent`
Set once at `modules/Runtime/src/Blueprint.cpp:547`; `ApplyMemberOverride`
(`:296-338`) never touches it; consumed at
`modules/Runtime/src/SceneSerializer.cpp:721` to decide placement composition.
Editor authors both directions (`EditHistory.cpp:409` delete, `:415` add).

*Failure A (remove `Parent`):* member reloads at its local offset instead of
composed — e.g. world (1,0,2) instead of (23,0,6), 22 m from its car, no log.
*Failure B (add `Parent`):* placement composed twice.
Both are load-time: the editing session looks correct, damage appears on reopen.
Same defect in the nested path (`Blueprint.cpp:502`).

### B4 Join → Stop leaves the host's instance rows in your level and saves them
`modules/Editor/src/EditorPlay.cpp:120-134` (capture) / `:300-330` (restore);
`modules/Editor/src/EditorNet.cpp:244-250`.
`_playSnapshot` records entities only. `BuildJoinedWorld` loads the host's level
into the **edited** world passing `&_world->instances`; `LoadFromFile` calls
`instances->Clear()` (`SceneSerializer.cpp:939-943`). `StopPlay` restores
entities, `levelPath` and `systemNames` — never `instances`.

*Failure:* after Join → Stop you are editing your level with the host's rows, all
`authored = true`. `InstancesForSave` (`Blueprint.cpp:209-224`) keeps them, so
the next Save writes the host's instances into your file and yours are gone. The
dirty marker never fires. Lesser variant with no networking: `SpawnBlueprint`
during play leaves non-authored rows forever — ghost billboards that
`PickInstance` selects and the Inspector shows as "0 live member(s)".

### B5 `[VERIFIED]` `ForgetEntities` is fed entity handles from *other* Scenes
`modules/Editor/src/EditorBlueprintMode.cpp:346` (`_worlds.ForEach`), `:431`,
`:451` (`CountForgettable`), `:505` (`ForgetEntities`).
Accumulates `doomedEverywhere`/`destroyedEverywhere` across **every resident
world**, then passes them to one history bound to one scene
(`ActiveHistory()`, `EditorApp.cpp:1093-1112`).
Entity handles are `(slot, generation)` with **no scene identity** — every scene
numbers from `{0,0}`. `NamesAny` (`EditHistory.cpp:131-144`) is a plain `==`.

*Failure (false positive):* a level-world member at `{7,0}` matches a
blueprint-world transaction at `{7,0}`; the modal reports a bogus count and
`ForgetEntities` erases that **suffix** — one accidental match at index 3 of a
12-deep stack throws away 4 steps of unrelated history.
*Failure (false negative, the one that matters):* the level world's history
genuinely names the destroyed members and is never truncated. Ctrl-Z later →
`ReviveAt` on a free-or-reoccupied slot (`EditHistory.cpp:556`; `Scene.hpp:80-90`
says free-slot-only), or a `ComponentDelta` whose `Scene::Add` silently no-ops.

Header documents the correct behaviour (`EditorApp.hpp:598-600`, "any world that
lost a member"); the implementation comment asserts the opposite
(`EditorBlueprintMode.cpp:445-446`). `TestEditHistory.cpp:769-772` names the
hazard and no test crosses scenes.

---

## 2. Silently wrong on the wire

### B6 `Relevance::ControllerOnly` is defeated by block escalation
`modules/NetSync/src/Replication.cpp:715-729` erases ControllerOnly members;
`:750-784` then re-adds **every** member of any block a surviving sibling belongs
to and re-intersects only against `_liveNetIds`. The filter is never re-applied.
`_controllerOnly` is read at exactly one site (`:715`), so there is no second gate
— contradicting both the comment at `:742-749` and the plan's R13.

*Failure:* a car with a ControllerOnly member (private HUD state, hidden
objective). Player B's provider names any wheel → B receives full state for that
member every keyframe. No log.

### B7 Record-erase and record-resend use different predicates → permanently bare mirrors
Client erases `_instanceRecords[start]` only when
`record->second.memberCount == length` for a run starting exactly at `base`
(`Replication.cpp:2810-2814`). Server resends when the instance is absent from
`knownInstances` (`:2097-2099`). Client on resend: `insert_or_assign` returns
`inserted == false` → `continue` → **never re-expands** (`:2741-2743`).
Despawns are RLE'd over the whole set (`:2148-2161`) and do not respect block
boundaries; `_nextNetId` only climbs so adjacent blocks are the normal case.

*Failure:* two 3-member instances at bases 1 and 4 leave relevancy together →
`despawns = {1..6}` → one run `(1,6)` → `memberCount(1)==3 != 6`, base 4 never
probed → neither record erased, all six entities destroyed client-side. On
re-entry the records resend, the client skips expansion, and the members arrive
as bare mirrors (`:2891-2906`). Because they arrive on an empty baseline, the
`MatchesAuthored` elision (`:1951-1956`) suppresses every component still equal
to the authored value — **never sent again**. Also reachable from a plain budget
starve (run of 3 vs memberCount 4).

### B8 A member pruned on the host is resurrected on every later joiner
`InstanceInfo::memberCount` is the definition's count, captured once at block
allocation (`Replication.hpp:339-345`, `:1240-1250`;
`App/src/BlueprintReplication.cpp:62-67`). Client expands all `memberCount` and
binds `base + i` (`Replication.cpp:2764-2767`). A member destroyed after spawn is
despawned only for connections that already **acked** it.

*Failure:* a client joining afterwards holds a live phantom member at
`base + prunedIndex` that no despawn names and no delta touches. Nothing on the
wire carries "which members exist". Same shape for level-authored `removed`,
which `WorldInstanceExpander::Expand` drops entirely
(`App/src/BlueprintReplication.cpp:139-146` sets no removals) while the host
leaves `NullEntity` holes (`SceneSerializer.cpp:555-562`).

### B9 `MatchesAuthored` compares against the *pre-placement* block
`modules/App/src/BlueprintReplication.cpp:74-121` compares live component bytes
against `definition->members[i].prepared[…].block`, which holds the **authored
local** value. But `SceneSerializer.cpp:716-725` composes the placement onto
every **parentless** member at expansion, so the live Transform is
`Compose(P, T)` while the comparison operand is `T`.

*Consequence 1:* at any non-identity placement nothing ever matches — the byte
saving simply never fires.
*Consequence 2 (silent):* if the live bytes coincide with `T` while
`P ≠ identity`, the component is elided on the empty baseline and **never
resent** (gate is `sinceChangeTick == 0 && !clientHasIt`,
`Replication.cpp:1951`). Reachable: a car placed with a non-identity rotation and
a member whose rotation is reset to identity before the first snapshot.

*The test that claims to measure this* (`modules/App/tests/TestBlueprintReplication.cpp:214-260`)
spawns at `{}` — identity placement — the one case where the comparison is both
effective and sound. It cannot catch either half.

*Note:* the null-codec-context concern was checked and is **not** a problem —
`PrepareBlueprint` also encodes with a null context (`SceneSerializer.cpp:1176`)
and `IsCodecLossless` (`:1094`) keeps `norep` components off the prepared path.
The placement is the hole.

### B10 32-bit wrap on attacker-controlled `base` overwrites a legitimate entity
`Replication.cpp:2764-2767` — `NetId{entry.base.value + member}`. `entry.base` is
only checked for `IsValid()` (`:2735`). With `base = 0xFFFFFFFE` and a 3-member
blueprint (so `members.size() == memberCount` passes), the third binding wraps to
`NetId{1}` and **overwrites a real entity's mapping**; later deltas for NetId 1
land on an instance member. Silent.
`:2795-2803` has the same wrap on `start.value + offset` (lesser: only destroys
mirrors, which the server heals).

### B11 The instance-record section ignores the snapshot byte budget
`Replication.cpp:2106-2130` writes every fresh record before the priority loop
and never consults `_config.maxSnapshotBytes` (default **1100**,
`Replication.hpp:119`). Each record ≈ 45-52 bytes (3 varints + 10 raw floats), so
~21 fresh instances exhaust the budget and the entity loop writes nothing.

*Failure:* joining a level with 100 placed instances → a single ~5 KB unreliable
snapshot (`:2318`), repeated every snapshot until acked. No pagination, no
"left for next time" path — it either fits or the join stalls. This is the
parking-lot case the feature exists for.

### B12 `instanceToWire`/`instanceFromWire` are installed on 1 of 7 codec sites
Installed at `Replication.cpp:1834` and `:2855` (the component path) only.
`entityToWire`/`entityFromWire` are installed at `1273, 1344, 1529, 1561, 2478,
2496` as well. An `AMSG` carrying an `ECS::InstanceId` — which
`tools/reflectgen/reflect_codegen.py:841` explicitly recommends — crosses
untranslated and names the **sender's** instance on the receiver. Silent.

### B13 `ForgetAcked` scrubs three of four cumulative sets in the in-flight ring
`Replication.cpp:883-902` scrubs `record.netIds`, `.written`, `.components`.
`SentSnapshot::instances` (`Replication.hpp:826-832`, filled at `:2181`) joined
the same wholesale install in `HandleAck` (`:1063`) but is **not** scrubbed; the
re-entry rule (`:810-819`) only erases from `connection.knownInstances`.

*Failure:* instance leaves and re-enters inside one round trip; the record is
written into a snapshot that is then lost; an ack for a pre-leave snapshot
reinstates `knownInstances = [I]`; the next snapshot computes
`freshInstances = {}` and never resends, while the client erased its record.
Members land as bare mirrors (B7's tail).

---

## 3. Not wired, dead, or unsafe

### B14 `[VERIFIED]` The `InstanceView` storage ban is evadable; `transient` *is* a hatch
`tools/reflectgen/reflect_codegen.py:845` — substring match on the *spelled*
type (`'InstanceView<' not in bare`). `_check_unsupported` then lets the field
past because `AFIELD(transient)` excuses an unknown type.

Reproduced: this generates cleanly, exit 0 —
```cpp
using CarView = Assisi::Runtime::InstanceView<Assisi::Blueprints::Car>;
ACOMP() struct ViaAlias { AFIELD(transient) CarView v; };
```
Also evades via template alias, `ACOMP(transient)` on the struct, and a member of
a member. `docs/blueprint-build-status.md:82-85` claims no hatch exists.

### B15 `[VERIFIED]` Blueprint replication has no production caller
`InstallInstanceInfoProvider` / `InstallInstanceExpander`
(`modules/App/src/BlueprintReplication.cpp:198`, `:204`) are called **only** from
`modules/App/tests/TestBlueprintReplication.cpp:103-104`. Nothing installs them
on a live `NetSession`. In a real session every member still replicates
individually. "End to end" is true of the test harness only.

*Blocker for wiring it:* `BlueprintReplication.hpp:15-18` requires the same
`ContentSet::paths` vector the handshake hashed, but `ContentSetHashJob`
(`ContentSet.cpp:121-138`) calls `BuildContentSetHash()`, which **discards
`paths`** and returns only the `uint64_t` (`ContentSet.hpp:100` is
`Core::Task<std::uint64_t>`). Whoever wires this is forced to rebuild the vector
— the thing the header forbids. Fix the job to carry the paths.

Related: `IndexOf` (`BlueprintReplication.cpp:26`) assumes sortedness
(`lower_bound`) and nothing validates it or that both sides agree. Two blueprints
with equal member counts → NetSync's `members.size() != memberCount` guard
(`Replication.cpp:2748`) passes and the client expands the **wrong file**.

### B16 `[VERIFIED]` `CancelSystemInstalls` is dead code guarding a live use-after-free
`modules/App/src/SystemCatalog.cpp:175`, declared `SystemCatalog.hpp:161-163`
("for a world that is about to be destroyed, whose queue entries would otherwise
name freed memory"). **No callers** — not `EraseWorld`, `Destroy`,
`DestroyAllExcept`, or `~WorldManager`.
The pending list is keyed by raw `World*` (`SystemCatalog.cpp:27`), and
`Application.cpp:517-528` runs `DrainMain` (where marshalled level loads free
worlds) one line before `DrainSystemInstalls()`.

*Failure:* F3 spawns a Bouncer → `QueueSystemInstall`; a marshalled level load in
the same `DrainMain` frees the outgoing world; `DrainSystemInstalls` dereferences
`pending.world` (`:171`). Also a test-order landmine —
`TestBlueprintReplication.cpp:320` calls `DrainSystemInstalls()` globally, safe
today only because no other `.abp` fixture declares systems.

### B17 Data race: the blueprint definition cache is an unsynchronised global
`modules/Runtime/src/Blueprint.cpp:249-253` — process-global `std::map`.
`SceneSerializer::LoadFromFile` runs on a **worker thread** in async travel
(`modules/App/src/World.cpp:310-330`) and reaches it via `StageInstance` →
`GetBlueprintDefinition`. The comment there still claims "No cache, no renderer,
no manager state" — untrue since blueprints landed. Main thread meanwhile calls
it from `FindMember`, `Save`, and per-frame from `EditorInspector.cpp:1270` /
`EditorApp.cpp:1067`. Concurrent insert+lookup on `std::map` is UB, and
`ClearBlueprintCache` invalidates `const BlueprintDefinition*` others hold.

### B18 `ServerApp` still exits 0 on five failed-start paths
`_startupFailed` set at only `apps/sandbox/src/ServerApp.cpp:100` and `:108`.
Missing: `:146-150` (`Host` fails), `:169-172` (`Join` fails), and the `fail`
lambda at `:181-186` used by all five join refusals including the
undeclared-system one (`:219-223`) and load failure (`:225`).

### B19 Typing in the instance Inspector pushes one undo transaction per frame
`modules/Editor/src/EditorGizmo.cpp:148-149` — `if (!nowUsing)
EndInstanceGesture(...)`, unconditional on the *gizmo* not being held.
`DrawTransformGizmo` runs before the Inspector in `OnImGui`
(`EditorApp.cpp:1386` vs `:1412`), so a drag in `DrawInstanceInspector` opens the
gesture on frame N and the gizmo closes it on frame N+1. 60 frames = 60
transactions; blows past `kMaxDepth = 256` and evicts real history.
The Inspector's own guarded close (`EditorInspector.cpp:1211`) is therefore dead
code. Needs the symmetric `&& !ImGui::IsAnyItemActive()`.

### B20 `LoadLevelFromPath` can return false with the scene already replaced
`modules/Editor/src/EditorLevels.cpp:751-755`. Near-unreachable now thanks to the
pre-check at `:719`, but if `ApplySystems` fails after `App::LoadLevel` succeeded,
the early return skips `SetPlayState(Editing)`, `ClearSelection()`, the
eyedropper/asset-browser disarm and `_history->Clear()` (`:759-789`) over a
freshly rebuilt scene. Every stored handle then aliases a different entity.

---

## 4. Codegen hardening

### B21 `[VERIFIED]` C++ keyword ban list has holes
`tools/reflectgen/blueprint_views.py:31-42` + `:171`. Missing: **`case`, `true`,
`false`, `xor`, `xor_eq`, `and_eq`, `or_eq`, `not_eq`, `bitand`, `bitor`,
`compl`, `co_await`, `co_return`, `co_yield`**. `default`/`delete`/`do`/
`operator` are present, so these are oversights of the same kind. An entity named
`case` emits `ECS::Entity case;` into a file marked "Do not edit".
Also `:365` checks the *type* name with `isidentifier() or in _CXX_KEYWORDS` but
**omits `keyword.iskeyword`** — `--blueprint class=a.abp` emits `struct class;`.

### B22 The generator is laxer than the loader in two dangerous places
`blueprint_views.py:61-69` (`_read`) and `:82-123` vs
`modules/Runtime/src/Blueprint.cpp:393` and `:438`:
- **`version`** — the loader throws unless `version == 2`; the generator never
  looks. A `version: 1` file generates a full view.
- **Uniform scale** — the loader throws on a nested instance with non-uniform
  `transform.scale`; the generator has no check.

Either way the build succeeds, call sites compile, and at runtime
`GetBlueprintDefinition` returns `nullptr` so **every** `SpawnBlueprint<T>`
returns `nullopt` forever. The manifest cross-check can't catch it — it only runs
on already-valid opted-in files.

*Everything else about the naming rules agrees exactly* — prefixes at every
depth, the `index >= first` removal scoping, the `/`-delimited cascade
(`Blueprint.cpp:346` ⟷ `blueprint_views.py:77`, including `car_` vs `car/`),
entities-before-instances ordering, dedup on full paths, cycle detection.

### B23 `kSource` interpolates the source path into a C++ literal unescaped
`blueprint_views.py:302`, `:266`. `Car=blueprints\car.abp` emits
`"blueprints\car.abp"`; a `"` in a path breaks the header.

---

## 5. Other findings worth folding in

- **`GetBlueprintDefinition` can throw where its contract says nullptr** —
  `Blueprint.cpp:583-604`: the `try` closes at `:595`, `PrepareBlueprint` is
  called at `:603` outside it and reaches generated `j.at(...).get<float>()`.
  Affects `FindMember`, `SceneSerializer::Save` (so the editor's Save can throw),
  and `ReexpandInstance`'s precondition check.
- **In-file `removed` orphans sibling references fatally**, while the same
  removal per-instance nulls them with a warning — `Blueprint.cpp:466-476` vs
  `SceneSerializer.cpp:553-563`. Contradicts §6.
- **Nested-file override values are never reference-qualified** —
  `Blueprint.cpp:478-503` never calls `QualifyReferences`. Loud (whole blueprint
  unusable) unless a same-named top-level entity exists, then silently wrong.
- **The documented "with a warning" on a dropped reference does not exist** —
  `SceneSerializer.hpp:275-277` promises it; `SaveEntitiesToFile` (`:1420-1449`)
  has no such pass. "Create blueprint from selection" silently drops every wire
  to an entity outside the selection.
- **Client instance rows leak** — no despawn counterpart to
  `WorldInstanceExpander::Expand`; NetSync can't reach `App::World::instances`.
  Dead `BlueprintInstance` rows accumulate and show in the outliner.
- **`World.hpp:190-202`'s `ApplySystems` doc contradicts the new implementation**
  (still says "clears first … left with no systems rather than some").
- **`ApplySystems` doesn't cancel that world's pending installs before
  `Clear()`** (`World.cpp:92`) — a queued blueprint's systems install into a
  level that never asked for them.
- **`PromotePendingLoad` passes an empty context** (`World.cpp:474`) so the error
  names no file; `_pending.reset()` at `:478` is dead (already reset at `:450`).
- **`_instanceBlocks` / `_blockRanges` are never pruned**
  (`Replication.hpp:1250`, `:1257`) and are keyed by a counter that restarts at 1
  on level load. Any path keeping a `ReplicationServer` across a level change
  reuses the old level's base/memberCount/placement.
- **Escalation is quadratic** — `Replication.cpp:753-772` pushes the whole block
  once per already-effective member. 100 cars × 20 members = 40k pushes + a 40k
  sort per connection per snapshot. `effective` is ascending and blocks are
  contiguous; a skip-ahead makes it linear.
- **Duplicate `(instanceId, memberIndex)` aliases silently** —
  `EnsureInstanceBlock` guards out-of-range (`:531-537`) but not duplicates.
- **`ReplicationClient::Reset()` no longer resets the session** —
  `Replication.cpp:3426-3458` leaves `_instanceRecords`, `_instanceIdByBase`,
  `_levelReady`.
- **`SetInstanceExpander`'s doc is false** (`Replication.hpp:1519-1521`): the
  server's elision can't know whether the peer installed an expander, so a client
  without one gets bare entities missing components — wrong, not "merely larger".
- **`WorldInstanceExpander::Expand` skips asset resolution** —
  `SpawnBlueprint` resolves meshes (`BlueprintVerbs.cpp:40-51`); the expander
  doesn't. Survives only because `EditorApp.cpp:956-965` re-resolves on a
  structure-revision bump. A Game build without that draws nothing.
- **`EndInstanceGesture` drops the record change when no member moved** —
  `EditorGizmo.cpp:258` (`txn.cmds.size() > 1`). An all-parented instance or a
  sub-epsilon nudge moves the row without recording it: not undoable, not dirty,
  but saved.
- **`CreateBlueprintFromSelection` anchors on a *local* Transform** and doesn't
  refuse a parented root (`EditorLevels.cpp:377-379`); `_selection.front()` need
  not be in `subtree` at all.
- **Instance-name uniqueness is half-enforced** — `EditorLevels.cpp:295-303`
  checks only other instances, not entity names, while `NameForOverrideTarget`
  addresses both in one namespace. `CreateBlueprintFromSelection` checks nothing.
- **A save while the re-expand prompt is up is silently swallowed** —
  `EditorBlueprintMode.cpp:337-338` returns before `InvalidateBlueprint`.
- **`PickInstance` ignores `_showEditorOverlays`** — invisible click targets.
- **`_submittedIconOutlines` grows unbounded** when the outline pass is invalid —
  `SceneRenderer.cpp:409-437`.
- **`levelInstanceIndex` is never renumbered on save** —
  `Blueprint.cpp:209-227`. Latent for the shared-baseline join.
- **`PlaceInstance`'s all-or-nothing unwind is a no-op if `StageInstance`
  throws** — `SceneSerializer.cpp:1215-1239`.
- **A blueprint declaring a `Name` component overwrites the member's leaf name**
  — the level path guards this, the blueprint path doesn't.
- **`HashTextFileNormalized`'s read-error check is dead** —
  `Core/src/ContentHash.cpp:17-19`; `istreambuf_iterator` sets neither
  `eofbit` nor `failbit`.
- **`FieldType::InstanceRef` falls through two switches** —
  `EditorInspector.cpp:491` renders "[unsupported type]";
  `BinaryCodec.cpp:724` skips via `default: continue`.
- **Stale comments:** `Replication.hpp:1528` and `Replication.cpp:2705-2707` say
  records are "not yet acted on — expanding one is 7c" (shipped);
  `World.hpp:459` profile note; `TestBounce.cpp:211` says "contains Bounce" when
  it contains `Counter`.

---

## 6. Tests that pass while the behaviour they name is broken

- `modules/App/tests/TestBlueprintReplication.cpp:214-260` — measures the byte
  saving at identity placement only (B9).
- `modules/NetSync/tests/TestBlueprintReplication.cpp:281` — "a member index
  outside the block is refused, not aliased" asserts
  `beyond.value >= base.value + 2`, which is **also true if aliased**. Needs `==`.
- Same `>=`-instead-of-`==` weakness at `:188`, `:228`, and `:245`.
- `tools/reflectgen/tests/test_reflectgen.py:363` — the transient-hatch test
  spells `InstanceView<Car>` literally and passes with B14 wide open. Its
  docstring states the exact claim it fails to defend.
- `test_reflectgen.py:375` — asserts `assertIn("instance", cpp)`, satisfied by the
  field *name*; never checks `FieldType::InstanceRef` or the codegen.
- `modules/App/tests/TestWorld.cpp:701-717` — declares `order` and never writes or
  reads it; delete the `After`/`Before` forwarding from `ApplyResolved` and it
  still passes.
- `TestBlueprintReexpand.cpp:156` — never exercises the `memberIndex` remap
  (`body` is index 0 before and after). A **reordering** case would.
- `TestBlueprintAuthoring.cpp:279` — asserts the null; the documented warning
  doesn't exist.
- `TestBlueprintOverrides.cpp:285` — asserts only `AliveCount() == 2`; would pass
  if the claim were applied to the wrong member.
- `TestBlueprintVerbs.cpp:139-148` — `DestroyInstance`'s Jolt teardown is
  unasserted; delete the `RemoveBody` call and it passes with bodies still
  colliding.
- `TestEditHistory.cpp` / `TestInstanceHistory.cpp` — all single-scene; both stay
  green with B5 fully present.
- Nothing covers: B6 (ControllerOnly + escalation), B7 (two adjacent instances),
  B8 (late joiner after a prune), B13 (stale ack), B1/B4/B19 (UI-level).
- `FieldType::InstanceRef` is not in the codec fuzz corpus
  (`TestBinaryCodec.cpp:700-750`), which plan risk 4 asked for.
- Mutation survivors in `test_blueprint_views.py`: dropping `keyword.iskeyword`;
  dropping the `index >= first` guard (unreachable in practice); emitting the
  manifest in sorted order (the Python suite never asserts on `kMembersOf*`).

---

## 7. Suggested order

1. **The two doc lies** (B1's doc, B14's doc) — they actively stop people looking.
2. **Data-corrupting editor bugs**: B1, B2, B3, B4, B5. B1 is two lines.
3. **B16** (dead `CancelSystemInstalls` → UAF) and **B17** (cache race) — both
   are crashes waiting rather than wrong pixels.
4. **Wire bugs**: B6-B13. The plan's own risk 3 says *land the tests before
   touching those sections*. B7 and B8 are the two the reviewer most wanted
   confirmed by a real test first.
5. **B15** — wire replication into a real session, which needs the `ContentSet`
   job to carry `paths`. Until then none of §2 is reachable in a real game.
6. **Codegen**: B14, B21, B22, B23.
7. **The vacuous tests** — this is what let all of the above through.

## 8. Checked and found fine (don't re-review)

`ComposeTransform`/`InverseComposeTransform`/`AuthoringOrigin` are exact inverses
under the uniform-scale rule; cycle detection; duplicate detection on full paths;
the removal cascade's `car/` vs `car_` distinction; member ordering as a pure
function of file bytes; the prepared form genuinely being a decode (vector
aliasing is properly tested); the merge lattice (outermost-wins, `null` removes,
removal beats an outer claim); `ReexpandInstance` preserving `(slot, generation)`;
two-phase naming in `Load`; protocol-8 framing symmetry and every client-side
bounds check on counts; the `SentSnapshot.instances`/`knownInstances` cumulative
discipline apart from B13; `_liveNetIds`' sorted insert and all its consumers;
`EnsureInstanceBlock` reached from both id paths; `BinaryCodec`'s `InstanceRef`
bounds discipline; the content-set handshake and its four states; all five
strong-id sentinel pairings and every `.value` boundary; the depfile actually
working (verified with `ninja -t deps`); `SystemCatalog::Resolve`/`ApplyResolved`
split and `ApplySystems`' resolve-before-clear; `ContentSet`'s sort-and-normalize;
`LevelSystemsAreDeclared`; `BuildSceneBodies` ordering; `EditHistory` truncation
arithmetic (the suffix rule is right); `ResetOverride`'s copy-before-`RestoreAt`;
`LoadLevelFromPath`'s refusal genuinely preceding both `ShutdownNetSession` and
the scene replacement.
