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
| 7b, 7c, 7d | `2a22ec5`…`d2388b7` | Blueprint replication, end to end (see "The network") |

## Not built

Nothing. **7b, 7c and 7d are built** — see "The network" below. The only item
still carried is D-C / `kExpansionVersion`, deferred by decision and not
blocking.

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

Storing a view in a reflected type is a build error, and **neither
`AFIELD(transient)` nor `ACOMP(transient)` is a way through** it, unlike the
ordinary unserializable-field check. The objection is not that it cannot
serialize — it is that its handles go stale, so a stored view is a member list by
another name.

The ban is on the type, not on its spelling, and two layers enforce it:

- **reflectgen**, which resolves the spellings visible in the header it is
  reading: an alias, an alias of an alias, a `typedef`, or a struct declared
  there that holds a view, all to a fixpoint. It reports the chain it followed.
- **the generated file**, which `static_assert`s every reflected field — the
  transient ones too — against `decltype`. This is what covers an alias declared
  in *another* header, which reaches a regex parser as an ordinary word.

**The edge that remains:** a view buried in a struct that some *other* header
declares. The generator does not read that header, and `decltype` sees the
wrapper, not what it holds. Closing that needs a compiler front end, not a
sharper regex — so if a member list ever turns up in disguise, look there first.

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

## The network

**The tag's translation is done** (`9147585`). `ECS::BlueprintMember` is
`ACOMP(replicable)`, and its `instanceId` — a per-world, per-machine counter — is
now rewritten at the codec boundary: out as the instance's base NetId, back in as
the receiving machine's own id. An instance never expanded locally resolves to
zero, so the tag is invalid rather than pointing at an unrelated instance. This
was the "inert by luck" hole earlier revisions of this file warned about.

Built so far, all under `TestBlueprintReplication.cpp`:

| Ref | What |
|---|---|
| R4, R7 | `InstanceInfoProvider`; members take one contiguous NetId block (`2a22ec5`) |
| R5, R3 | The record on the wire, section ordering, **protocol 8** (`fabfb4a`) |
| R6 | Despawn run-length encoding; the empty record section costs one bit (`3b89834`) |
| 7c core | `InstanceExpander` and the `base + i` binding (`d34b2e0`) |
| 7c tag | The `instanceId` ↔ `baseNetId` translation (`9147585`) |
| 7d | Block escalation — naming one member pulls the instance (`e66ba06`) |
| App halves | The provider and the expander, over a real blueprint (`db3af9b`) |
| 7b saving | A member matching its blueprint costs no component bytes (`3bfcc81`) |
| 7d re-entry | Re-entry inside a round trip resends the record (`285f305`) |

**The saving is real and measured**, in `modules/App/tests`: the same scene
replicated with and without the manifest, and an edited member costing more than
an untouched one. Elision is **by value against the prepared block**, never by
change tick — a tick says when something was written, so a component written
back to its authored value, or written in the same tick the instance was
created, would be judged wrongly, and a wrongly skipped unchanging component
stays wrong for ever.

An overridden member is never elided: the override lives in the level, which a
mid-session joiner never read.

Mirrored members get **no authority physics**. The expander calls
`SceneSerializer::PlaceInstance` and deliberately not `App::SpawnBlueprint` —
the verb also builds Jolt bodies, and a member that simulated locally would
argue with the server every tick. Motion arrives as body state and the client
raises a mirror body through the ordinary path.

A joining client installs the systems a blueprint names (`d2388b7`, R14) — the
cross-machine half of the founding "system never installed" failure, W1 in the
concept review. Same call the host's spawn makes, queued for the next safe
point, idempotent.

**Still open:** D-C / `kExpansionVersion`, which was deferred by decision and is
not blocking. Blueprint replication itself is done.

**Before merging to `dev`:** `kNetProtocolVersion` is 8 and the bump is
refuse-to-join by design — a v7 client reads the record count as its despawn
count and desyncs the whole stream. Anything running the old build will not
connect.

A failed expansion refuses the snapshot and logs why; carrying that reason into a
disconnect the user sees needs the session layer and is not wired.

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
- The Levels panel's Save wrote to whatever the *combo* had selected rather than
  the level that was open — load `Test`, scroll to `Materials`, save, and
  `Materials.alvl` got `Test`'s contents, your own edits went nowhere, and
  `SaveLevelToPath` then retargeted `_world->levelPath` at `Materials` and marked
  the editor clean. This entry previously claimed it was "fixed on the way past";
  it was not, and the line was byte-identical to `dev` until round 7 caught it
  (B1). The button now calls `SaveLevelToPath(_world->levelPath)` directly, and
  is disabled for a world that has no path yet — Save As is how one gets named.
- Run used to capture the edited world's level path and system list and nothing
  else, so Stop put those back and left the **instance table** wherever the
  session had moved it (B4). A join loads the host's level into the edited world,
  which clears the table and refills it with the host's rows, all `authored` — so
  Join → Stop left you editing your own level with somebody else's instances in
  it, no dirty marker, and the next Save wrote them to your file over your own. A
  `SpawnBlueprint` during play was the same bug without the networking: a row
  that outlived the session as a ghost the outliner drew with no live members.
  The three fields are now one `PrePlayState` (`modules/Editor/src/PrePlayState.cpp`),
  captured and restored in one call each — the table goes back **whole**, ids and
  allocator included, because an allocator that regressed would hand out an id an
  undoable delete still has a claim on. `modules/Editor/tests/TestPrePlayState.cpp`
  covers the restore; the by-eye pass was walked on 2026-08-07 — a dedicated host
  on a level with one placed instance, an editor joining it, Stop, Save: the file
  came back with no instances, and the session's own log shows the join and the
  `StopPlay` that followed it.
- Instance names were unique by the caller's good manners rather than by rule
  (S17): "Place instance" stepped a colliding name to `car_1`,
  `CreateBlueprintFromSelection` checked nothing, so two selections saved as
  `turret.abp` in one level placed two instances called `turret`. Both claim
  `turret/…`, which the loader refuses outright — a level that saves and never
  reopens. The rule now lives in two places that cannot drift: `PlaceInstance`
  **refuses** a non-empty name already live (the one door both gestures come
  through), and `Runtime::UniqueInstanceName` is what the editor calls first so
  an author gets `car_1` rather than a refusal. Unnamed instances are exempt and
  must stay so — a runtime spawn and a replicated mirror both pass no name, and
  refusing the second bullet would break replication.

  **S17's stated mechanism is wrong and the tests now say so.** It reported that
  an instance may take a name already belonging to an *entity*, leaving
  "two things answering to one name". They do not collide: the entity claims
  `car` and the instance claims `car/body`, and no reference can mean both.
  `TestBlueprintExpansion.cpp` pins that as legal, alongside the three collisions
  that are real — two instances of one name, two entities of one name, and an
  entity whose name spells a member path (`car/body`). The last two came in
  through the Inspector's rename box, which validated nothing, and are fixed here
  too.
- **`Runtime/Naming.hpp` is now the one statement of what may be called what.**
  Two rules: a name holds no `/`, and a name is unique among its own kind. The
  second rule falls out of the first — given no name contains a separator and
  every member path does, entity names and member paths are disjoint *by
  construction* rather than by anyone remembering to cross-check them.

  One stateless walk (`UniqueName`) serves both namespaces through
  `UniqueEntityName` and `UniqueInstanceName`. Stateless matters: a counter that
  remembered what it handed out would drift the moment a name went unused, an
  object was deleted, or an undo ran — offering `car_5` in a level with no cars.
  It replaces `CreateEntity`'s open-coded copy of the same loop, which is where
  the rule had drifted in the first place. Side effect worth knowing: the first
  new entity is now `Entity` rather than `Entity_1`, matching the first car being
  `car`.

  `ValidateName` returns `std::expected<void, NameError>`, so the rename box says
  *why* under the field and load puts the same reason in its refusal. Load still
  signals by throwing — `SceneSerializer::Load` returns void and generated
  component deserialization throws from nlohmann regardless — so the conversion
  happens at that boundary; three throws there collapsed into one sourced from
  the rule. Taking exceptions out of `Load` itself is a separate pass.

  A rename is **refused**, not auto-suffixed, unlike a new entity's name: a fresh
  entity has no name worth keeping, but a rename is something the author typed on
  purpose, and quietly storing `crate_1` for `crate` is an edit they did not make.
- `ReexpandInstancesOf`'s "one collection at a time" guard used to sit at the top
  of the function and `return` (S18). Everything below it was skipped —
  **including `InvalidateBlueprint`**, which has nothing to do with the prompt.
  So a save arriving while an earlier prompt was up wrote the file and left
  `GetBlueprintDefinition` handing out the contents from *before* the write, with
  nothing logged: editing a blueprint and losing the edit looked identical. The
  guard now sits below the collection and the invalidation, and takes the
  declining answer on the author's behalf rather than returning quietly — the
  write stands, the cache is honest, and the source goes into
  `_staleInstanceSources`, which refuses hosting until the copies catch up or the
  level is reloaded. That is exactly the prompt's own "Leave them", so both now go
  through one `MarkInstancesStale`.

  **The prompt this guards had never once opened**, which is how it went a whole
  review round without anyone noticing what the guard did. `ReexpandInstancesOf`
  measured the undo cost through `ActiveHistory()`, and
  `EditHistory::CountForgettable` refuses a scene it is not bound to. A
  blueprint-mode save destroys members in the *level* worlds while
  `ActiveHistory()` is `_blueprintHistory`, bound to the blueprint world — so the
  count came back 0 every time, `ApplyPendingReexpand` ran straight through, and
  the author was never asked. `ForgetEntities` then declined on the same test, so
  the level's `_history` was left holding transactions naming entities that had
  just been destroyed: dangling handles, and after a dense rebuild an undo that
  writes into whatever entity inherited the slot.

  Both sites now ask **every** history via `AllHistories()` rather than the active
  one. No world→history map is needed and none was added: `CountForgettable` and
  `ForgetEntities` each take a scene and return 0 for one they do not own, so the
  cross product is self-filtering and stays correct however many histories there
  come to be. Asking "where do I record this edit" and "what does this edit cost"
  were the same call, and they are not the same question.
- The confirmation itself was written as a receipt — titled "Blueprint saved",
  leading with what was written and mentioning the history third. That is the
  shape an author dismisses by reflex, which for this dialog means throwing away
  undo history without reading. It now leads with the cost in the warning colour,
  the destructive button says what it does (`Update copies and drop the history`),
  and `SetItemDefaultFocus` sits on `Cancel` so a stray Enter lands on the answer
  that loses nothing.

  A warning *sound* was tried and dropped. Windows has `MessageBeep`, one call
  with no dependency; Linux has no way to make a noise without either a link
  dependency (libcanberra, libpulse) or launching a player and babysitting it,
  and neither is worth it for a chime on a dialog that is already modal, centred
  and titled with its cost. The place for it is an audio module — miniaudio-style
  backend plus a synthesized two-tone chime, no asset and no theme lookup — and
  the call belongs at the same point in `ReexpandInstancesOf`, once per prompt
  rather than once per frame.
