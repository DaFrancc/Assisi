# Replication Authoring & Editor Integration Plan

Status: **planned, not started**. This is the v3 networking plan. It exists because
v2 (networking-design-notes.md) built a working transport and delta-snapshot
backend but shipped no usable *feature*: the live two-editor test connected and
replicated nothing, because the plan staged subsystem layers instead of a
user-visible capability. This plan is organized the other way around — every
milestone ends with something you can see work in the editor, and the backend
changes exist to serve that.

## 0. Why the live test failed (the three gaps)

1. **Nothing is marked.** Replication is opt-in per entity (`Replicated`
   component) and no level, tool, or default ever adds it. Hosting a level
   replicates zero entities. The panel even says so — in grey text, after the
   fact.
2. **The client has no world.** Join does not tell the client which level the
   host is running. The client sits in whatever scene it had open; mirrors (if
   any arrived) would float in an unrelated world.
3. **Mirrors would be invisible anyway.** The client applies `MeshRenderer`
   GUID fields but never resolves them (`ResolveSceneAssets` is only called on
   level load / reimport), so a mirrored entity has no `meshBuffer` and draws
   nothing.

Independently, the authoring model is underspecified: any reflected component
on a marked entity travels; there is no way to say "this component type is
networkable" or "this field is not".

## 1. The authoring model (how the user talks to the system)

Three filters, ANDed. Each answers a different question, none substitutes for
another:

| Layer | Question | Mechanism | Default |
|---|---|---|---|
| Entity | does *this instance* travel? | `Replicated` component (exists today) | not replicated |
| Component type | *can* this type travel? | `ACOMP(replicated)` | not replicated |
| Field | does this field go on the wire? | `AFIELD(norep)` opt-out | replicated |

Rationale, in one line each:

- **Entity marker stays opt-in**: most of a level is static scenery both sides
  load from the same file; replicating it restates what nobody changes. The
  fix for "nothing was marked" is editor UX (§3) and the level handshake (§2),
  not inverting the default.
- **Type opt-in (`ACOMP(replicated)`)**: matches every surveyed engine (Unreal
  `bReplicates`, Source SendTables, O3DE NetworkProperty archetypes, Mirror
  NetworkBehaviour). It is what stops `Camera`, editor bookkeeping, and future
  gameplay-local components from leaking onto the wire the day someone marks
  an entity.
- **Fields default-in with `norep` opt-out** (not opt-in per field): inside a
  type someone deliberately marked networkable, a forgotten annotation should
  cost visible bandwidth, not silently-unsynced state. `worldMatrix` needs no
  treatment — unannotated fields are invisible to reflection already, and
  `AFIELD(transient)` fields (resolved pointers) are already excluded from
  serialization and stay excluded from the wire.

### reflectgen / metadata changes

- `ACOMP(replicated)` — parsed alongside `tracked`/`transient`. **Implies
  `tracked`** (delta replication is change-tick driven; a replicated-but-
  untracked component is a contradiction, so the generator supplies the hooks
  rather than making the user write both).
  Errors: `replicated` + `transient` is a hard generation error.
- `AFIELD(norep)` — valid only inside an `ACOMP(replicated)` type (elsewhere it
  is a dead annotation, which is worse than an error, so it *is* an error);
  `norep` + `transient` is an error (already excluded).
- `ComponentMeta` gains `bool replicated`; per-field metadata gains a
  `replicated` flag (packed into the existing field-flag storage). Golden
  files regenerate.
- `Core::Reflect::ProtocolHash()` folds in the per-type replicated flag and the
  per-field rep mask, so two builds that disagree about *what* replicates
  refuse to pair at `ServerHello` instead of desyncing quietly.

### Initially marked types

- `ECS::Transform` → `ACOMP(replicated, tracked)` (position/rotation/scale;
  `worldMatrix` already excluded by being unannotated).
- `Runtime::MeshRenderer` → `ACOMP(replicated)` (GUID fields travel; transient
  resolved pointers already excluded — this type is the proof the model works
  with zero `norep` annotations).
- `Runtime::Name` → `ACOMP(replicated)` (mirrors get real names in the
  hierarchy panel instead of anonymous rows).
- **Not** `Camera` (local concern), **not** `RigidBodyDescriptor` (mirrors are
  visual; the server owns physics — replicating the descriptor would tempt the
  client into simulating a body it doesn't own), **not** `Parent` (its payload
  is an `EntityRef`; entity references need NetId remapping on the wire, which
  is real design work — deferred, see §6; mirrors are flat in v1).
- `NetSync::Replicated` itself does **not** wire-replicate: `priority` is a
  server-side send concern. The client adds the marker to mirrors locally (it
  already does — that is how disconnect cleanup finds them).

### Replication server/client changes

- `ReplicationServer::WriteEntityComponents`: skip components whose meta lacks
  `replicated` (today: everything serializable travels). Field masks are built
  from replicated fields only. The removal set (`ackedComponents` diff) only
  ever contains replicated component ids, so its logic is unchanged.
- `ReplicationClient`: additionally tags each mirror with a new
  `ACOMP(transient) NetSync::Mirrored` tag — the editor's hook for "this
  entity belongs to the session, render it, don't let the user edit it".

## 2. The join experience (level handshake + visible mirrors)

**Level handshake.** `ServerHello` gains the host's level virtual path (empty =
none). On accept:

- Editor client: if its current level differs, load the host's level. The
  existing `LoadLevelFromPath` tears down the net session (deliberately — a
  load replaces the scene the session is bound to); it gains a "requested by
  the session" path that skips that teardown and instead rebinds the client's
  scene reference. Order: load level → strip (below) → apply snapshots.
- Headless client (`ServerApp`): `LoadLevelSim` with the same path.
- Client with unsaved edits: prompt before discarding (editor already has this
  gesture for level loads); declining aborts the join.

**Duplicate prevention.** An entity authored in the level *and* marked
`Replicated` would exist twice on the client: once from its own level load,
once as a mirror. Rule: **a net-client level load strips level entities that
carry `Replicated`** — the server's copies are authoritative and arrive as
mirrors. (Unreal's equivalent: level-placed replicated actors are owned by the
server.) Static scenery (unmarked) stays local and costs zero bandwidth.

**Making mirrors draw.** `ReplicationClient` sets a dirty flag whenever it
creates an entity, adds a component, or writes a durable `AssetId` field.
`NetSession::ConsumeStructureDirty()` exposes it; the editor checks it after
`PollNetSession()` and runs `Runtime::ResolveSceneAssets` (the async-load
machinery already upgrades placeholder→mesh as loads land, and the existing
`HasPendingLoads` re-resolve loop keeps polling). No physics rebind — mirrors
get no Jolt bodies. Verify `PropagateTransforms` covers mirrors in Editing
state (it should — it runs unconditionally in the update systems).

**Already handled** (kept, with tests): mirror teardown on disconnect, session
teardown on user-initiated level load, host keeps running when a client drops.

## 3. Editor UX

- **Inspector**: a "Replicated" checkbox in the entity header (adds/removes the
  marker through the normal edit-capture path, so it is undoable and marks the
  level dirty). Component headers on a replicated entity show a small wire
  glyph for `ACOMP(replicated)` types — you can see what will travel.
- **Mirrored entities** (`Mirrored` tag): hierarchy rows tinted + icon;
  inspector shows a banner ("Mirrored from server — read-only") and disables
  editing (server would overwrite the edits within a snapshot anyway; showing
  editable fields that snap back reads as a bug).
- **Network panel**: when hosting with zero replicated entities, an amber
  warning with the fix in it ("no entities are marked Replicated — check
  'Replicated' in the inspector"). Show the negotiated level path on both
  sides. Existing stats stay.
- **Semantics note** (documented in the panel tooltip + design notes): hosting
  works in Editing *and* Play states — the net pump deliberately runs outside
  the `IsSimulating()` gate. Editing-while-hosting is a live one-way preview;
  clients see edits to replicated entities as they happen.

## 4. PIE-style multi-process testing (the Unreal route)

The toolbar Play control gains a small net dropdown: **Standalone** (default,
today's behavior) / **Host + 1 client** / **Host + 2** / **Host + 3**.

On `StartPlay()` in a host mode:
1. Host the session on the first free port in 27015–27039 (session flagged
   `ownedByPlay`).
2. Spawn N child processes of the same executable (`/proc/self/exe`) with
   `--pie-client 127.0.0.1:<port>`: a full editor instance that auto-joins on
   startup (level arrives via the §2 handshake, so it needs no `--load-level`),
   titled as a PIE client. A slimmed windowed game client is Phase-2 template
   work (Game/GameEditor targets), not this plan.

On `StopPlay()`: disconnect the play-owned session (a manually-hosted session
is left alone), SIGTERM the children, SIGKILL any survivor after a grace
period, reap. Children set `PR_SET_PDEATHSIG` first thing under `--pie-client`,
so an editor crash cannot leak windows.

New small utility: `App::ChildProcess` (spawn argv / terminate with grace /
reap; POSIX now, interface portable for the Windows pass).

## 5. Milestones — each with a user-visible definition of done

**M1 — Wire gating** (reflectgen `replicated`/`norep`, ComponentMeta/field
flags, ProtocolHash, replication filters, mark Transform/MeshRenderer/Name,
`Mirrored` tag).
*DoD*: tests prove an unmarked type never travels, a `norep` field stays at
its client-side default while sibling fields update, disk serialization still
round-trips `norep` fields, and two builds with different annotations refuse
the handshake. Full suite green.

**M2 — The join that works** (ServerHello level path, client level load +
Replicated-strip, structure-dirty asset resolve, no-physics-for-mirrors).
*DoD*: editor A hosts `Materials.alvl` with one entity marked Replicated and
moving in Play; editor B, opened with *no* level, joins → the level appears,
the entity is visible and moving smoothly, nothing is duplicated; B
disconnects → mirrors vanish, level stays. I run this end-to-end myself
(windowed + headless variants) before handing it over.

**M3 — Authoring in the editor** (inspector checkbox + wire glyphs, mirrored
read-only + hierarchy tint, panel warning + level display).
*DoD*: an entity can be made to replicate with one click and no code; hosting
an unmarked level warns loudly; a mirrored entity cannot be edited.

**M4 — PIE processes** (net dropdown, ChildProcess, `--pie-client`,
StopPlay teardown).
*DoD*: Play with "Host + 1 client" opens a second window that auto-connects
and shows the moving world; Stop closes it and ends the session; repeat 3×
with no zombie processes (`ps` verified) and no port-in-use failure.

**M5 — Verification + docs**: full self-driven E2E (host editor + windowed PIE
client + headless client simultaneously; disconnect/reconnect; level switch
mid-session tears down cleanly), a lag/loss pass via the loopback fake-lag
mode, then update networking-design-notes status, remaining-work.md §1, and
the project memory.

Order: M1 → M2 → (M3 ∥ M4) → M5. M2 is the milestone that retires the "it
doesn't actually work" complaint; M1 exists because M2 without it replicates
every serializable component again.

## 6. Explicitly deferred (unchanged from v2 unless noted)

- **Client→server gameplay changes** (ownership, client edits traveling,
  prediction): the system stays one-way server-authoritative. Editing on a
  client does not propagate — the read-only mirror UX makes that visible
  instead of surprising.
- **RPCs**: own milestone after this plan; validation is security-critical and
  deserves design attention, not a rider.
- **EntityRef fields on the wire** (Parent/hierarchy replication): needs NetId
  remapping in the codec. New in v3's deferred list.
- Relevancy/interest management and actually using `Replicated::priority`;
  quantization hints on the wire; Windows build (protobuf_MSVC_STATIC_RUNTIME
  landmine noted in remaining-work.md).
