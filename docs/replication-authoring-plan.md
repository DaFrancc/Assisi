# Replication Authoring & Editor Integration Plan

Status: **planned, not started**. This is the v3 networking plan, revised after a
two-reviewer design round. It exists because v2 (networking-design-notes.md)
built a working transport and delta-snapshot backend but shipped no usable
*feature*: the live two-editor test connected and replicated nothing, because
the plan staged subsystem layers instead of a user-visible capability. This
plan is organized the other way around — every milestone ends with something
you can see work in the editor, and the backend changes exist to serve that.

## 0. Why the live test failed (the three gaps)

1. **Nothing is marked.** Replication is opt-in per entity (`Replicated`
   component) and no level, tool, or default ever adds it. Hosting a level
   replicates zero entities. The panel even says so — in grey text, after the
   fact.
2. **The client has no world.** Join does not tell the client which level the
   host is running. The client sits in whatever scene it had open; mirrors (if
   any arrived) would float in an unrelated world.
3. **Mirrors would be invisible anyway.** The client applies `MeshRenderer`
   GUID fields but never resolves them. `ResolveSceneAssets` runs on level
   load/reimport and every frame *while any async load is pending*
   (`UpgradeStreamingAssets`) — the latter can intermittently mask this bug,
   which is why it looked flaky rather than absolute. A mirrored entity created
   with no load in flight has no `meshBuffer` and draws nothing.

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
  rather than making the user write both). This also fixes a live bug: today's
  untracked `MeshRenderer`/`Name` transmit at spawn but never delta afterwards
  (`ChangedById` reads 0 = "unchanged" for untracked pools).
  Errors: `replicated` + `transient` is a hard generation error.
- `AFIELD(norep)` — valid only inside an `ACOMP(replicated)` type (elsewhere it
  is a dead annotation, which is worse than an error, so it *is* an error);
  `norep` + `transient` is an error (already excluded).
- `ComponentMeta` gains `bool replicated`; `FieldMeta` gains a `bool
  replicated` member (FieldMeta uses discrete bools, not a packed flag word —
  one more positional member in the generated aggregate initializers, golden
  files regenerate).
- `Core::Reflect::ProtocolHash()` folds in the per-type replicated flag and the
  per-field rep mask, so two builds that disagree about *what* replicates
  refuse to pair at `ServerHello` instead of desyncing quietly. (The wire is
  parse-safe either way — field masks are on the wire and authoritative — so
  refusal is policy, not corruption avoidance.) `ProtocolSummary()` names the
  replicated types/fields, so a mismatch log says *which* annotation differs.

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
  client into simulating a body it doesn't own), **not** `Parent` — deferred,
  see §6. Note the honest reason: EntityRef remapping on the wire **already
  works end-to-end** (CodecContext entityToWire/FromWire, server remap,
  client `_pendingRefs` deferred patching, all tested). Parent is deferred for
  *hierarchy semantics* — a mirrored child of an unreplicated local parent,
  strip interactions, transform-space questions — not for missing codec
  machinery. M1's filters make that machinery dormant; its tests stay green
  so it is ready when Parent is designed.
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
  Transient means it never saves and, being invisible to the serializable-only
  play snapshot, would silently vanish across a client's Play/Stop — which is
  one of the reasons a connected client cannot press Play (§2).

## 2. The join experience — a client is a *session viewer*, not a co-editor

The review found that the v2 draft of this section patched symptoms of one
underlying confusion: it treated a joined editor as an editor. The redesign is
one rule: **while joined, the client's scene is a session view.** It is built
fresh from disk, stripped, fed by snapshots, cannot be saved, and is rebuilt
from disk when the session ends. Every sub-decision below falls out of that.

**Level identity bookkeeping (new, unlisted in v2).** The editor currently
records nothing about which level is loaded. `EditorApp` gains a current-level
virtual path (set by `LoadLevelFromPath`, cleared on new/empty scene), plumbed
into `NetSession` so `ServerHello` can carry it. `ServerHello` also carries a
**content hash of the level file** as saved — a path alone cannot detect that
the two machines' files differ.

**Handshake ordering (the gate).** Today the client sends `ClientHello`
immediately on `ServerHello`, the server marks it ready, and snapshots stream
the same tick — but an editor level load must be marshalled to the next
frame's main-thread drain. Unfixed, snapshots would spawn mirrors into the
pre-load scene, and the load's `Scene::Clear` + dense re-create would leave
`_entityByNetId` holding stale handles that can alias live entities and pass
`IsAlive` — subsequent deltas and the eventual disconnect `Reset()` would then
write into / destroy arbitrary level entities. So:

1. On `ServerHello`: verify protocol, then *defer* `ClientHello` until the
   level is in. (Watch GNS idle-timeout during the load; the load is one frame,
   so this is a note, not a design problem.)
2. The join **always loads the level from disk**, even when the client already
   has the same path open. This closes two holes at once: the
   same-level-already-open case (which would otherwise skip the strip and
   duplicate every level-authored replicated entity) and the
   dirty-client-scene case (client always starts from the file the hash
   describes). After the load, verify the content hash; mismatch aborts the
   join with a clear panel message. Load failure (file absent) likewise aborts
   the join — no floating mirrors in an unrelated world.
3. After the load: **clear** the client's `_entityByNetId`,
   `_transformHistory`, and `_pendingRefs` (a map-clear API, *not* `Reset()` —
   Reset destroys the entities the stale handles now alias). `NetSession`
   cannot rebind its scene reference and does not need to: `App::LoadLevel`
   mutates the same `Scene` object in place; only the identity maps go stale.
   `LoadLevelFromPath` gains a "requested by the session" path that skips the
   usual session teardown and performs this invalidation instead.
4. Strip (below), then send `ClientHello`; snapshots begin.

**Client with unsaved changes.** No modal (none exists in the editor today —
v2 cited a prompt that was never built). The network panel shows the pending
join with a "Join — discards unsaved changes" confirm button; nothing loads
until clicked. PIE clients are fresh processes and skip this entirely.

**Duplicate prevention.** After the (unconditional) load, **strip level
entities that carry `Replicated`** — the server's copies are authoritative and
arrive as mirrors. (Unreal's equivalent: level-placed replicated actors are
owned by the server.) Static scenery (unmarked) stays local and costs zero
bandwidth. The strip is safe *because* the load just came from disk: it never
destroys user work. Ordering detail: the load path builds Jolt bodies for the
whole level (`RebindSceneAssetsAndPhysics`), so the strip must run before the
physics rebuild — or trigger a second one — to avoid stale bodies for entities
that no longer exist.

**Save is disabled while joined.** Mirrors are ordinary entities with
serializable components (including `Replicated`); one Save on a joined client
would bake the server's world — at interpolated poses — into the shared level
file, and the stripped level would overwrite the authored one. The Save/Save As
buttons disable with a tooltip while the session is in the client role, same
gesture as the existing play-mode save guard.

**Play is disabled while joined.** A client pressing Play would snapshot
mirrors into the play snapshot, roll their fields back on Stop to values the
server will never re-stamp (deltas resend only on server-side change), lose
the transient `Mirrored` tag across the revive, and simulate local physics
against a server-fed world. v1 answer: the Play button disables on a joined
client with a tooltip. Host-side Play/Stop while hosting is supported and was
verified safe: `ReviveAt` preserves handles so the server's NetId maps stay
coherent, `Add` restamps change ticks so clients receive the restored values,
and play-spawned entities despawn via the `IsAlive` sweep.

**Disconnect reloads from disk.** After the strip, the mirrors are the
client's *only* copies of level-authored replicated entities; disconnect
destroys mirrors, so "mirrors vanish, level stays" would quietly leave a level
missing entities the file contains — with a clean title bar, since the
session load reset the dirty token. Under the session-view rule the fix is
simple: when a client session ends (either side), the editor reloads the level
from disk. The scene is whole, clean, and truthfully matches the file.

- Headless client (`ServerApp`): `LoadLevelSim` with the same path + the same
  strip and map-clear; no UI concerns.

**Host with unsaved edits.** The handshake replicates a file, not the host's
memory: unmarked entities with unsaved host edits will silently differ on
clients. v1 accepts this with a loud amber warning in the host's network panel
whenever hosting (or accepting a join) with a dirty scene — "clients load the
last saved version; save to sync static scenery." PIE avoids the problem
structurally (§4, temp snapshot). Blocking or auto-saving manual hosts is
deferred until cross-machine use is real.

**Making mirrors draw.** `ReplicationClient` sets a dirty flag whenever it
creates an entity, adds a component, or writes a durable `AssetId` field.
`NetSession::ConsumeStructureDirty()` exposes it; the editor consumes it **once
per frame in `OnUpdate`** (next to `UpgradeStreamingAssets`), not after
`PollNetSession()` in the fixed step — `ResolveSceneAssets` is O(scene) and the
fixed step can run several times a frame during a join burst. (If the join
burst ever shows up in profiles, `ReadComponent`'s currently-discarded
`appliedMask` out-param enables per-touched-entity resolve; optimization, not
v1.) No physics rebind — mirrors get no Jolt bodies. `PropagateTransforms`
runs in the render path, which covers a windowed client; a headless client
never propagates, which is fine because nothing there reads `worldMatrix`.
Minor: `Interpolate` should skip its `GetMut<Transform>` write on the
hold-last-pose paths once the pose is applied, so idle mirrors keep the
hierarchy dirty-skip.

**Already handled** (kept, with tests): mirror teardown on disconnect, session
teardown on user-initiated level load, host keeps running when a client drops.

## 3. Editor UX

- **Inspector**: a "Replicated" checkbox in the entity header (adds/removes the
  marker through the normal edit-capture path, so it is undoable and marks the
  level dirty). Component headers on a replicated entity show a small wire
  glyph for `ACOMP(replicated)` types. The glyph's *absence* is not enough
  feedback, so on a marked entity each non-replicated component also gets one
  line of subdued text ("not replicated — type lacks ACOMP(replicated)"), and
  a marked entity with **zero** replicated components gets an inline warning
  (today it would spawn an empty, invisible mirror and inflate the panel's
  count reassuringly).
- **Parented-child warning**: `Transform` is parent-relative and `Parent` does
  not replicate in v1, so a marked child appears on clients at its local
  coordinates read as world space. The checkbox is the cheap place to catch
  it: marking a parented entity shows "this entity is parented; it will
  replicate at its local transform — unparent it or mark the root."
- **Mirrored entities** (`Mirrored` tag): hierarchy rows tinted + icon;
  inspector shows a banner ("Mirrored from server — read-only"). Read-only is
  enforced at **one shared predicate** (`IsEditable(entity)` ≈ not Mirrored)
  checked by *every* write path — inspector fields, the transform gizmo (which
  would otherwise fight the per-frame interpolation writes), the Delete key,
  the Entities panel delete/create/parenting actions — and mirrors **never
  enter the edit history** (an undo that revives a fake mirror the client no
  longer maps, or double-maps when the server respawns it, is worse than no
  undo). Note the rationale: this is not cosmetic — the server only resends
  fields it re-stamps, so a client edit to a static field would diverge
  *permanently*, not snap back.
- **Network panel**: when hosting with zero replicated entities, an amber
  warning with the fix in it ("no entities are marked Replicated — check
  'Replicated' in the inspector"). Show the negotiated level path on both
  sides, the dirty-host warning (§2), and the pending-join confirm (§2).
  Existing stats stay.
- **Semantics note** (documented in the panel tooltip + design notes): hosting
  works in Editing *and* Play states — the net pump deliberately runs outside
  the `IsSimulating()` gate. Editing-while-hosting is a live one-way preview of
  *replicated* entities only; edits to unmarked scenery do not travel (that is
  what the dirty-host warning is about).

## 4. PIE-style multi-process testing (the Unreal route)

The toolbar Play control gains a small net dropdown: **Standalone** (default,
today's behavior, and the default every run — the selection is not sticky) /
**Host + 1 client**. Host + 2/3 are deferred (§6): each PIE client is a full
editor process with its own Vulkan device, swapchain, and level asset set —
"host + 3" on one GPU makes the "moving smoothly" DoD unjudgeable under
contention. Host + 1 is the workhorse; a headless client can be added by hand
for load testing.

On `StartPlay()` in host mode:
1. **Serialize the current (pre-play) scene to a temp level file** in the
   scratch/session directory. This is the PIE answer to host-memory vs
   client-disk: the client loads exactly what the host is playing, unsaved
   edits included (Unreal's temp PIE packages, same reason). The play-owned
   `ServerHello` carries this file's absolute path + hash — PIE clients are
   same-machine by construction, so an absolute path is fine there and never
   used for manual joins.
2. Host the session on the first free port in 27015–27039 (session flagged
   `ownedByPlay`). Play-with-clients requires no session to be active; the
   dropdown's host modes disable while a manual session exists (one session
   per editor, no coexistence rules to invent).
3. Spawn 1 child process of the same executable (`/proc/self/exe`) with
   `--pie-client 127.0.0.1:<port>`: an editor instance that auto-joins on
   startup (level arrives via the §2 handshake), titled "PIE Client".

`--pie-client` is a **restricted viewer**, not a second editor:
- **No shared-file writes**: skip the startup reimport (read-only asset
  database — three processes racing over `.aast`/`.amat` sidecar writes is a
  corruption lottery), no `imgui.ini` write (per-process ini path in scratch,
  or none), no `options.json` write (an F11 in the PIE window must not rewrite
  the host's graphics options).
- **Save and Play disabled** (they already are for any joined client, §2; the
  flag makes it unconditional).
- **Framed for watching**: minimal panel layout, and the camera auto-frames
  the replicated entity set after the first snapshot burst — an Unreal user
  reads "PIE" as "play as a client", and what v1 actually delivers is a live
  view of the server's world; the window should look like that, not like a
  second editor pointed at the same level (which is exactly the artifact that
  caused the original collaborative-editing confusion). Possession/game-client
  windows are Phase-2 template work (Game/GameEditor targets), not this plan.

On `StopPlay()`: disconnect the play-owned session, SIGTERM the children,
SIGKILL any survivor after a grace period, reap, delete the temp level file.
Children set `PR_SET_PDEATHSIG` first thing under `--pie-client`, so an editor
crash cannot leak windows.

New small utility: `App::ChildProcess` (spawn argv / terminate with grace /
reap; POSIX now, interface portable for the Windows pass).

## 5. Milestones — each with a user-visible definition of done

**M1 — Wire gating** (reflectgen `replicated`/`norep`, ComponentMeta/FieldMeta
flags, ProtocolHash/Summary, replication filters, mark Transform/MeshRenderer/
Name, `Mirrored` tag).
*DoD*: tests prove an unmarked type never travels, a `norep` field stays at
its client-side default while sibling fields update, disk serialization still
round-trips `norep` fields, and differing annotations produce differing
ProtocolHashes (a unit test on the hash — the handshake-refusal path is
exercised by the existing mismatch test; an actual two-build handshake is not
automatable in one suite). Full suite green.

**M2 — The join that works** (level bookkeeping + ServerHello path/hash,
deferred ClientHello gate, always-load-from-disk + strip + map invalidation,
save/play disabled while joined, disconnect reload, structure-dirty resolve in
OnUpdate, no-physics-for-mirrors).
*DoD*: editor A hosts `Materials.alvl` with one entity marked Replicated
(hand-edited into the .alvl — the checkbox arrives in M4) and moving in Play;
editor B joins **twice, from both starting states** — with no level open, and
with the *same* level already open — and both times the level appears, the
entity is visible and moving smoothly, and nothing is duplicated; while
joined, B's Save and Play buttons are disabled; B disconnects → mirrors
vanish and the level reloads whole (the stripped entities are back, scene
clean). I run this end-to-end myself (windowed + headless variants) before
handing it over.

**M3 — PIE processes** (net dropdown, temp scene snapshot, ChildProcess,
`--pie-client` restrictions, StopPlay teardown).
*DoD*: with *unsaved* host edits, Play with "Host + 1 client" opens a second
window that auto-connects and shows the moving world *including the unsaved
edits*; the PIE window frames the replicated entities without hunting; Stop
closes it and ends the session; repeat 3× with no zombie processes (`ps`
verified), no port-in-use failure, and no writes to imgui.ini / options.json /
asset sidecars by the child (mtimes verified).
*Ordering rationale*: M3 lands before the authoring UX because it converts
every subsequent verification — all of M4, M5, and every future networking
change — from manual two-window choreography into one click. The `--pie-client`
flag plus §2's joined-client guards make it safe to hand out windows before
the full mirror read-only UX exists.

**M4 — Authoring in the editor** (inspector checkbox + wire glyphs +
non-replicated-component note + parented warning, mirrored read-only via the
single predicate + hierarchy tint + history exclusion, panel warnings + level
display).
*DoD*: an entity can be made to replicate with one click and no code; marking
a parented entity warns; hosting an unmarked level warns loudly; a mirrored
entity cannot be edited by the inspector, gizmo, Delete key, or hierarchy
actions, and never appears in undo history.

**M5 — Verification + docs**: full self-driven E2E (host editor + PIE client +
headless client simultaneously; disconnect/reconnect; level switch mid-session
tears down cleanly; dirty-host warning and hash-mismatch abort exercised), a
lag/loss pass via the loopback fake-lag mode, then update
networking-design-notes status, remaining-work.md §1, and the project memory.

Order: M1 → M2 → M3 → M4 → M5, strictly serial (solo developer; "parallel"
milestones are decoration). M2 retires the "it doesn't actually work"
complaint; M1 exists because M2 without it replicates every serializable
component again.

## 6. Explicitly deferred (with the honest reason)

- **Client→server gameplay changes** (ownership, client edits traveling,
  prediction): the system stays one-way server-authoritative. The read-only
  mirror UX makes that visible instead of surprising.
- **RPCs**: own milestone after this plan; validation is security-critical and
  deserves design attention, not a rider.
- **Parent / hierarchy replication**: deferred for hierarchy *semantics*
  (mirrored children of local parents, strip interaction, transform spaces) —
  the EntityRef wire machinery itself already exists and stays tested. Mirrors
  are flat in v1.
- **Host + 2/3 PIE clients**: dropdown extension is trivial; deferred until
  one-GPU contention is measured so the perceptual DoDs stay judgeable.
- **Dirty-host resolution beyond a warning** (auto-save on join, or shipping
  the host's in-memory scene to remote clients): PIE covers the solo-dev case
  via the temp snapshot; cross-machine needs a level-transfer design.
- **Per-entity asset resolve** via ReadComponent's appliedMask: only if the
  join-burst O(scene) resolve shows up in profiles.
- Relevancy/interest management and actually using `Replicated::priority`;
  quantization hints on the wire; Windows build (protobuf_MSVC_STATIC_RUNTIME
  landmine noted in remaining-work.md).
