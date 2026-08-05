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

## Not built

### 5d — re-expansion as an in-place diff, and history truncation

Deliberately last of stage 5. The editor has no live `.abp` editing session: a
blueprint is edited by opening it *as* a level, so there is nothing for
re-expansion to hang off yet. Build the session first; the in-place diff is
meaningless without it.

### 5e — the Play/Host flush gate

Its stated failure — "a green handshake over two different worlds" — is **not
reachable today**. The host expands from disk through `GetBlueprintDefinition`
exactly as a client does, so both read the same stale bytes and agree. It becomes
reachable the moment 5d makes the editor expand from an in-memory form.

The plan says to land 5e with 7a so the hash never ships without the flush; that
ordering assumed 5d was already there. **Land it with 5d instead.** The existing
dirty-level modal (`EditorNet.cpp`'s `DrawHostUnsavedModal`) already covers the
level case.

### 7b–7e — blueprint replication

Nothing started. See the warning below.

### 8 — `ASYSTEM` and the catalog

Nothing started. The format still carries `profile`, which is what the build
order intends: `systems` joins the format at stage 8, and converting four level
files twice is cheaper than blocking on codegen.

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
That is exactly what 7e fixes.

---

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
