# Blueprints — build status

Where `docs/blueprint-implementation-plan.md` actually stands on the `blueprints`
branch, what was decided while building, and what is left. Written so the next
person to open this does not have to re-derive any of it from the diff.

Every commit listed is green on `make gd` and `make gcc-ship`, 13/13 ctest in
both, and the four `assets/levels/*.alvl` load through the headless server with
no warnings.

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

## Not built

### 5d — re-expansion as an in-place diff, and history truncation

The last gap in the authoring loop. A blueprint is edited by opening it *as* a
level and saving it; nothing then tells the instances of it in other resident
worlds to catch up, so "edit the blueprint, see it everywhere" currently needs a
level reload.

The decided trigger is **save**: `EditorApp::SaveLevel` invalidates the saved
file's definition and re-expands every instance whose closure contains it, in
every resident world. It has to be an in-place diff rather than destroy-and-
recreate — entity handles are `(slot, generation)`, `EditHistory` stores exact
handles, and `Scene::ReviveAt` is valid only for a currently-free slot under a
strictly linear history, so recreating forty cars behind undo's back means a
later Ctrl-Z revives into a slot something else now occupies. When an edit does
delete a member, truncate the history below the oldest transaction naming any of
the destroyed entities.

### 5e — the Play/Host flush gate

Its stated failure — "a green handshake over two different worlds" — is **not
reachable today**. The host expands from disk through `GetBlueprintDefinition`
exactly as a client does, so both read the same stale bytes and agree. It becomes
reachable the moment 5d makes the editor expand from an in-memory form.

The plan says to land 5e with 7a so the hash never ships without the flush; that
ordering assumed 5d was already there. **Land it with 5d instead.** The existing
dirty-level modal (`EditorNet.cpp`'s `DrawHostUnsavedModal`) already covers the
level case.

### 7b, 7c, 7d — blueprint replication

Nothing started. See the warning below.

**7e is cancelled** by decision (2026-08-05): `BlueprintSpawnFromLevel` was
provisional by design and gets deleted when `replication-plan-v4.md` §5's
shared-baseline join lands, so building it means building something to throw
away. The join rework replaces it.

### 9 — `InstanceView<T>` codegen

Nothing started.

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
the two are separate knobs. Worth knowing that rotation goes the other way — a
crate authored at 45° comes back axis-aligned. Say so if that should change too.

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
