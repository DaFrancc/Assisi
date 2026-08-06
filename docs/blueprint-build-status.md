# Blueprints — build status

Where `docs/blueprint-implementation-plan.md` actually stands on the `blueprints`
branch, what was decided while building, and what is left. Written so the next
person to open this does not have to re-derive any of it from the diff.

Every commit listed is green on `make gd` and `make gcc-ship`, 14/14 ctest in
both, and the four `assets/levels/*.alvl` load through the headless server with
no warnings. (14, not 13: stage 9 adds a `blueprint-views` Python suite.)

---

## Built

| Stage | Commit | What landed |
|---|---|---|
| 0 | `06b665c` | Physics stops ignoring `Parent` — `PhysicsWorld::ParentWorldFn`, `App::BuildSceneBodies` |
| 1 | `24972b2` | Format v2: named entities, references by name, no v1 reader |
| 2 | `4f65f91` | Expansion, `ECS::BlueprintMember`, the world's instance table, `instances` in the format |
| 3 | `75a2e71` | Overrides and removals, and the merge lattice |
| 4 | `08ae1e3` | The four verbs and the two queries |
| 6 | `d3c41f8` | The prepared form — one codec block per component per member |
| 5a–5c | `9791e47` | `InstanceDelta`, outliner instance rows, the instance gizmo |
| 7a | `9897fa9` | The content-set hash and the handshake it gates |
| 5 authoring | `d5d5ac7` | The Blueprints panel: place an instance, create one from a selection |
| 8 | `4cbc649` | `ASYSTEM`, the catalog, and profiles deleted outright |
| 5b rest | `83b28eb` | Override marks, per-field and per-component reset, auto-naming |
| review 1 | `48a6ea3` | Reset stayed one press behind; multi-select; a blueprint's scale went into the placement |
| 5d, 5e | `c1c2091` | Blueprint editing mode, re-expansion on save, history truncation, the stale-copy host gate |
| 9 | `4231176` | `InstanceView<T>` codegen, the typed verbs, and the reflectgen ban on storing one |
| ids | `9253e33`…`ddf322d` | The five weak id aliases became strong types (see below) |

## Not built

### 7b, 7c, 7d — blueprint replication

Nothing started. See the warning below. **This is the only blueprint work left.**

**7e is cancelled** by decision (2026-08-05): `BlueprintSpawnFromLevel` was
provisional by design and gets deleted when `replication-plan-v4.md` §5's
shared-baseline join lands, so building it means building something to throw
away. The join rework replaces it.

---

## Stage 9, and the one thing that can rot in it

The generator is `tools/reflectgen/blueprint_views.py`, reached through
`reflectgen --instance-views`. It emits, per opted-in blueprint: the
`InstanceView<T>` specialization (nested exactly as the file nests),
`InstanceViewTraits<T>` carrying the source path so a typed spawn takes no
string, and a free `FillInstanceView`. The opt-in list is stated in the top-level
`CMakeLists.txt` — deliberately not a glob, because this is the one place content
reaches into the build graph. A **depfile** carries the nested files, which the
build cannot know about without reading a blueprint first.

**The rot risk:** member names are now produced by two implementations of one
rule — `blueprint_views.py` at build time and `Blueprint.cpp`'s `FlattenInto` at
run time. A divergence does not fail to compile. It resolves a field to
`NullEntity`, and the car simply has no wheels.

The guard is the manifest: the generated header ends with
`kGeneratedInstanceViews`, the member list this build believes in, and
`TestInstanceViews`' first case walks it against `GetBlueprintDefinition` for
**every opted-in blueprint** — order included, because a member's index is what
its NetId is assigned from. Opting a blueprint in is therefore enough to cover
it; nobody has to remember to write a case.

The fixtures under `modules/App/tests/blueprints/` are chosen, not arbitrary:
`parking_lot.abp` declares an entity `car_body` beside an instance `car` with a
member `body` (the collision that grouping avoids and flattening would not), and
`depot.abp` removes `car` from the lot, which must cascade to `car/*` while
leaving `car_body` alone — the `car/` vs `car_` distinction.

A view is move-only. Note this contradicts the concept doc's other description of
it as "a plain aggregate of handles": in C++20 a deleted copy constructor costs
the type its aggregate-ness. Move-only won, because the doc reasons explicitly
about `optional` holding a move-only view.

Storing a view in a reflected type is a build error with **no `transient`
escape hatch**, unlike the ordinary unserializable-field check. The objection is
not that it cannot serialize — it is that its handles go stale, so a stored view
is a member list by another name.

---

## The id types

`ECS::InstanceId`, `NetSync::NetId`, `Net::ConnectionId`,
`Reflect::ComponentId` and `Reflect::MessageId` are now strong types rather than
`uint32_t` aliases: aggregates with a public `value`, no constructor (so
aggregate initialization is the only way in, which blocks the implicit
conversion in both directions), no arithmetic, plus `std::hash` and
`std::formatter`.

**Their sentinels are not uniform, and that is deliberate.** `ComponentId` uses
`~0` and treats **0 as a valid id** — it is an ordinal that indexes `_pools`
directly. The other four use 0 as invalid, so zeroed memory names nothing.
`MessageId` is one-based for exactly that reason. Do not "unify" `IsValid`
across them; each definition says why it is what it is.

`FieldMask` was left a plain `uint64_t` on purpose: it is a bitmask, not an id
space, so a strong type would mean re-exporting the bit operators for no safety
gain.

---

## Read this before touching the network

`ECS::BlueprintMember` is `ACOMP(replicable)` — which the design requires (§2:
membership is state with a current value) — but the `instanceId` ↔ `baseNetId`
translation that makes it *mean* anything on a client is 7b/7c work. So today a
joining client receives a tag whose `instanceId` is the **server's** per-world
counter, which names nothing on its side.

This is inert rather than wrong: the whole design rule is that no system reads
the tag, and the editor's outliner falls back to "instance (?)" for an id its
table does not know. But it is inert by luck, not by construction. If you want it
safe before 7b lands, dropping `replicable` from the tag is one word, and 7b puts
it back with the translation beside it.

The other half of the same gap: the join's strip is still per-entity, so a level
that places instances will duplicate their non-replicated members on a client.
With 7e cancelled, the fix comes with the shared-baseline join — where level
content stops travelling at all and there is nothing left to duplicate.

---

## What the editor can do today

Open the **Blueprints** panel.

- **Place instance** — pick any `.abp` or `.alvl` and drop a copy in front of the
  camera. Undoable as one gesture.
- **Edit** — opens that file in its own world with an editor sun and a raised
  ambient, and switches the editor into *blueprint mode*. Saving writes the file
  and brings every live copy of it up to date in place, in every resident world
  that is not simulating. See "Blueprint mode" below.
- An instance is a first-class thing to select: its root draws the same billboard
  a placement-only entity does, clicking that billboard selects the instance, and
  the Inspector shows the **record** — name, source, live member count, and a
  placement you can type into rather than only drag. Nearest-hit wins between a
  root icon and an entity, so neither hides the other.
- The active entity of a multi-selection — the last one clicked, which is what the
  Inspector shows and the gizmo drives — outlines redder than the rest
  (`Runtime::kActiveSelectionOutline`). A single selection is its own active
  entity, so an ordinary click gets that colour too.
- **Create from selection** — name it, and the selected entity plus everything
  parented under it is written to `blueprints/<name>.abp` and replaced with an
  instance of it. The swap is invisible on screen; one Ctrl-Z takes it back.
- The **Entities** panel groups an instance's members under one collapsible row.
  Clicking the row selects the *instance* — the gizmo then moves the whole group
  and writes its placement, recording no member overrides. Expanding it and
  clicking a member selects that member.
- Selection is a list: Ctrl-click picks and drops rows, Shift-click takes a
  range, and in the viewport both modifiers add one more (there is no row order
  out there to draw a range through). The gizmo moves everything selected; Delete
  takes it all as one transaction; Create from selection writes all of it.
  Selecting an *instance* is exclusive — it is a different gesture, not a
  member of the same list.
- Editing a member records an override, marked in the inspector with a reset at
  either scope. `X` cycles the gizmo frame World → Local → Instance on a member.
- Nesting is not authored from the UI yet: a selection containing a member is
  refused, with a note pointing at the `instances` entry that expresses it.

## Blueprint mode

A blueprint is an ordinary level file, so editing one is opening it as a level.
What makes it a *mode* rather than another Open Level is that **the level you came
from stays resident behind it** — which is not a convenience. Saving the blueprint
has to bring that level's copies of it up to date in place, and it cannot do that
to a world it just unloaded.

The blueprint world takes the **edited** role while it is open, with its own undo
history and its own dirty marker, and hands the role back on close. Two live
histories rather than one saved and restored: the level's has to come back
untouched, and `EditHistory` binds a `Scene` by reference.

- **The rig** is a real entity carrying `Runtime::EditorOnly`, which
  `SceneSerializer::Save` skips exactly as it skips a blueprint member. So the sun
  is in the outliner and the inspector edits it like anything else, and no file
  ever sees it. Ambient is a renderer knob rather than a component — it was
  `const float kAmbient = 0.03` in `cube_min.frag` and is now a frame constant,
  defaulting to that same value everywhere, so no existing render changed.
- **Hidden while in there**: play control, the network panel, levels, the
  placement panel, Chiara, diagnostics. Not for tidiness — every one of those acts
  on *the level*, and the level is not what is in front of you. Play is refused
  from the hotkey too, since simulating a world holding one crate and a sun would
  settle its bodies into a pose the file then remembers.
- **The world selector is inert** in the mode: stepping out of it with the
  selector would leave the edited role behind on the blueprint, so the level you
  switched to would be view-only for a reason nothing on screen explains.

### Re-expansion (5d)

`EditorApp::SaveLevelToPath` writes the file, then `ReexpandInstancesOf` walks
every resident world for instances the edit reaches — **by closure, not by path**,
because a parking lot's flattened member list contains the car's members. A
simulating world is skipped and named in the log.

`SceneSerializer::ReexpandInstance` is an in-place diff, matched by member
**name**: a name in both lists keeps its exact `(slot, generation)`, a name only
in the old list is destroyed, a name only in the new one is created. It has to be
a diff rather than destroy-and-recreate because `EditHistory` stores exact handles
and `Scene::ReviveAt` is valid only for a free slot — rebuild forty cars behind
undo's back and a later Ctrl-Z revives into a slot something else now occupies.
The previous member names must be captured *before* the cache is invalidated;
nothing can reconstruct them after.

The instance's record is untouched: placement, overrides and removals belong to
the level that placed it, not to the file being edited.

### Truncation, and the prompt

When an edit deletes a member, `EditHistory::ForgetEntities` drops undo steps —
**as a suffix, not a filter**. Undo replays newest-first, so if step 12 names a
dead handle then nothing older than 12 is reachable either; the newest offending
transaction and everything below it goes. Dropping only the offenders would leave
steps that apply against a `before` state which was never restored.

Because that can cost a lot of unrelated history, a save that would delete members
**asks first** — naming them and the number of steps at stake. The prompt is about
the catch-up, not the save: the file is written either way, because which members
die is only knowable from the new definition, which only exists once the file is
on disk. Declining leaves the live copies where they were before 5d existed.

### The stale-copy gate (5e)

Declining is what makes 5e's failure reachable, and it did not exist before.
Leave copies stale, then host: a client expands the file fresh and builds a
different member set under the same NetIds, and the content-set hash agrees the
machines match — because it hashes the *disk*, and the disks do match. So hosting
is refused while anything is stale, with the fix named in the message. A refusal
rather than a prompt: unlike unsaved edits there is no "host it anyway" that means
anything, since the copies are wrong either way. Cleared by a level load or by an
accepted catch-up.

## Decisions taken

The plan's three open items were resolved with its own recommendations:

- **D-A** — the content-set hash gates PIE uniformly. **Implemented**: a PIE
  child computes the same hash and waits on it like any other join. A second
  handshake path is the thing the strict check exists to avoid.
- **D-B** — undo reviving a member mid-session is allowed. Nothing built so far
  depends on it; the per-member NetId array it needs is 7b work.
- **D-C** — fold a `kExpansionVersion` into `NetProtocolHash()`. **Not yet
  implemented** — it belongs with 7b, where expansion order first becomes
  protocol surface.

## Calls made where the docs did not settle it

**The file-level `name` is backed by `Runtime::Name`.** The brief shows `name` as
a sibling of `components` but never says where it lives at runtime. One name per
entity keeps the editor's outliner and inspector correct with no change, and
avoids a display label sitting beside an identity that disagrees with it. Cost:
every level entity now carries a `Name`, which is replicable — a one-time ~40
bytes per entity on spawn. Level entities load from file on both sides, so the
join path does not pay it.

**An override naming a member the blueprint no longer declares is dropped with a
log line, not refused.** The brief §6 says both, two paragraphs apart, and the
plan repeats both. They cannot both hold, because the loader cannot tell a
deleted member from a mistyped one. Implemented the rule that comes with a stated
reason — "banning renames is what makes deletion the only reading". The brief is
worth one line of correction here.

**A per-instance removal leaves a hole in the member list; a removal authored *in*
a file really removes.** The first has to, because the index is the NetId offset
and two instances of one file that removed different members must still agree
about which index names which member. The second cannot vary per instance, so
there is nothing for a hole to preserve. The brief implies this without saying it.

**The origin a selection is authored around carries no scale** (`AuthoringOrigin`).
Position and rotation cancel into the placement, scale does not. Nothing said
which; passing the whole transform divides the members' scale out, so the copy
replacing the original looks right and every fresh instance comes back the wrong
size. The rule taken: where a thing stands and which way it faces is placement,
how big it is is what it is. An instance's own scale still multiplies on top, so
the two are separate knobs.

Rotation deliberately goes the other way — a crate authored at 45° comes back
axis-aligned — and that was **confirmed as the wanted behaviour** (2026-08-05),
not left as an accident. A scale of 0.6 is set because that is how big a crate
is; a rotation of 37° is usually just where the thing landed when it was
dropped. Cancelling the common case is right more often than preserving it. If
a group's *whole* facing is meaningful — a staircase assembly on a diagonal —
give it a dummy root at identity and tilt the child: only the root's rotation is
dropped, children keep theirs.

**A blueprint gets a world, not a window.** "Open the blueprint editor in a new
window" was asked for and is not what shipped: ImGui multi-viewport is off
(`DebugUI.cpp`), and a second OS window needs per-viewport swapchains in the
Vulkan backend — a renderer project larger than 5d itself. A dedicated world plus
a reduced panel set gives everything else that was wanted. The one thing lost is
dragging it to another monitor.

**Warn about the catch-up, not about the save.** The prompt was specified as
Cancel/Save. It is Update/Leave instead, for two reasons: which members die is
only knowable from the *new* definition, which only exists once the file is
written; and cancelling a save the author explicitly asked for is worse than
leaving live copies stale, which is a state the editor already had.

**A component with an `AFIELD(norep)` field keeps the JSON path** rather than
being encoded into the prepared form. The codec skips `norep`, which is right for
the network and wrong for a file — where the file *is* disk. Nothing declares one
today; the check is there so the day one appears it costs a little speed rather
than a silently missing field.

## Smaller loose ends

- NetSync's mirror body creation passes no `ParentWorldFn` — it cannot see
  `Runtime::Parent`. Harmless until a mirrored entity is parented, which is 7c.
- `SceneSerializer::Save` logs an error if it sees blueprint members with no
  instance table. Every call site passes one; the log guards against a new one
  that forgets.
- The four `.alvl` files were converted to v2 by a one-shot JSON transform (a
  throwaway, not committed — it preserved components the converter's binary would
  not have had registered). They round-trip through the engine now.
- The truncation test pins the surviving stack's *depth and label*, not that it
  still replays. "Leaves a replayable stack" is what stage 5d asks for and only
  the shape of it is checked.
- 5d's by-eye check is unpaid: drag a car, save, confirm the file holds zero
  member overrides.
- The Levels panel's Save used to write to whatever the *combo* had selected
  rather than the level that was open — load `Test`, scroll to `Materials`, save,
  and `Materials.alvl` got `Test`'s contents. Fixed on the way past; `SaveLevel`
  now goes through `SaveLevelToPath(_world->levelPath)`.
