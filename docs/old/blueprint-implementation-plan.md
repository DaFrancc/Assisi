# Blueprints — implementation plan (v2)

A re-review of `docs/blueprint-system-concept.md` against the code on `dev`,
and the staged plan for building it. This replaces the previous version of this
file, whose five open decisions (D1–D5) have since been resolved — several
against its recommendation — and are **settled**: two spawn-record flavours
with `BlueprintSpawnFromLevel` provisional (D1, see `replication-plan-v4.md`
§5, lines 1111–1148); `BlueprintMember` replicates as `ACOMP(replicable)` with
the wire carrying `baseNetId` (D2 reversed); no gate on mid-session blueprint
editing, but Play/Host flushes unsaved blueprint edits (D3); the client sends
one whole-content-set hash in `ClientHello` and blueprints are named on the
wire by sorted-list index (D4); and **no new module** — the format stays with
level loading in Runtime, the tag goes where NetSync can see it, and NetSync
gets injected seams instead of a link (D5 reversed). None of those are
re-opened here. What survives from the previous plan is retained explicitly;
everything that assumed `modules/Blueprint` or a manifest exchange is gone.

Every claim about existing code cites file:line in this tree, re-verified this
pass.

---

## 1. Verdict

**Buildable.** The five decisions integrate with the code as it stands, and
the one that had to be load-bearing — the D5 seam — checks out concretely:

- The client applies a snapshot in one synchronous pass whose sections are
  despawns → entity blocks → body states → events
  (`Replication.cpp:2346-2588`), and it already mutates the scene mid-Poll
  (mirror creation at `Replication.cpp:2442-2456`), so a spawn record expanded
  *inline* by an application-installed function is the same class of operation
  the path already performs. GPU asset resolve is already deferred through
  `StructureRevision` (`Replication.hpp:1277-1285`) — mirrors arrive with
  unresolved assets today — and system installs already have a safe point at
  `DrainMain` (`Application.cpp:509-518`). Nothing about expansion has to
  happen asynchronously, so nothing needs the once-per-join deferral
  machinery. The seam is a pair of injected functions (§2 R4 below), the
  `PhysicsWorld::ParentWorldFn` shape §12 of the brief already mandates for
  physics.
- `ACOMP(replicable)` on the tag composes with everything: replication is
  gated per entity by the `Replicated` marker (`Replication.cpp:446-458`,
  `1405-1426`), so the tag on a non-replicated member costs nothing; the
  protocol hash picks the new type up automatically
  (`BinaryCodec.cpp:790-825`); relevancy and priority never enumerate
  component types. The one genuinely new mechanism is the `instanceId` ↔
  `baseNetId` translation, which is **new codec surface** — the codec has
  exactly two per-field hooks today, both for `EntityRef`
  (`BinaryCodec.hpp:121-126`) — designed in §2 R2.
- The hash-in-`ClientHello` design fits the handshake with one bump
  (`kNetProtocolVersion` 6→7, `NetProtocol.hpp:153`) and one new
  `RejectReason`; the async-hash requirement fits the job system
  (`Core::JobSystem::Run` → `Task<T>::IsComplete`, `JobSystem.hpp:117-196`)
  with one server-side care: `ClientHello` is sent exactly once and never
  resent (`Replication.cpp:2068-2088`), so a server that is not ready must
  withhold its `ServerHello` rather than ever drop a hello (§2 R9).

Three items need an owner decision (§3). None blocks stages 0–6; two sit
inside stage 7 and one is a one-line hash question. Everything else found was
trivially resolvable and is adopted in §2.

The re-review also found one **live latent bug** the plan fixes in passing:
the editor's level hash folds CRLF (`EditorNet.cpp:92-127`) and the headless
server's does not (`ServerApp.cpp:45-61`) — the same file hashes differently
across the two hosts on a CRLF checkout today, blueprints or no blueprints
(§2 R10).

Honest size: stages 0/1/4 are days; 2/3/6 about a week together; 5, 7 and 8
are each a week-plus. This is a multi-week build even with the parallelism in
§4 exploited.

---

## 2. Inconsistencies — resolved

Adopted; build from these without reopening them.

### R1. Where the tag lives: `modules/ECS`, beside `Transform`

D5 removed the new module, but NetSync must still *read* the tag with types —
it links `Core + ECS + Math + Net + Physics` and deliberately not Runtime
(`modules/NetSync/CMakeLists.txt:37-45`), and it resolves component ids via
`registry.IdOf(typeid(T))`, which needs the header
(`Replication.cpp:287, 2063-2065`). The precedent is exact:
`ECS::Transform` is an `ACOMP(replicable, tracked)` living in ECS with its own
reflection pass (`ECS/Transform.hpp:36`, `modules/ECS/CMakeLists.txt:30-34`).

**Adopted:** `BlueprintMember` is `ACOMP(replicable)` in a new
`modules/ECS/include/Assisi/ECS/BlueprintMember.hpp`, added to ECS's
`assisi_reflect` list. Runtime (expansion), Editor, and NetSync all reach it
render-free. The instance *table* stays in Runtime with the expansion code —
NetSync never sees its type; it reaches instance-level facts through the R4
seam. This matches the brief's own framing of the tag as "same tier as
`NetId`".

### R2. `instanceId` ↔ `baseNetId` is new codec surface — `AFIELD(instanceRef)`

There is no existing per-field translation hook for a plain integer field:
`CodecContext` carries exactly `entityToWire`/`entityFromWire`, both consumed
only for `FieldType::EntityRef` (`BinaryCodec.hpp:121-126`;
write side installed at `Replication.cpp:1642-1650`, read side at
`2393-2410`). The decided design — wire carries `baseNetId`, client maps to
its local id — therefore needs:

- a new field attribute, `AFIELD(instanceRef)`, parsed by reflectgen into a
  `FieldMeta` flag (the `controlled` attribute at `FieldMeta.hpp:118-138` is
  the template);
- `CodecContext::instanceToWire` / `instanceFromWire` hooks, applied to
  flagged `UInt32` fields in `WriteComponent`/`ReadComponent`;
- one line folding the attribute into `ProtocolLayoutDescription`
  (`BinaryCodec.cpp:790-825`), so two builds disagreeing about it refuse to
  pair — the same rule that already makes `transient`/`norep` flips a
  protocol change (`BinaryCodec.hpp:14-21`, `FieldMeta.hpp:69-81`).

Server side, `instanceToWire` reads the per-instance net record (R7); client
side, `instanceFromWire` reads the `baseNetId → local instanceId` map held by
`ReplicationClient`. Note this hook is **not** a spawn-only nicety: every
keyframe sweep re-sends full state including the tag
(`ResetBaselines`, `Replication.cpp:924-932`), so the translation runs
regularly, and the locally-resurrected-mirror path
(`Replication.cpp:2422-2440`) rebuilds a bare member whose only correct tag
value arrives through it. An id that fails to map decodes to 0 with a warning,
same convention as an unresolvable `EntityRef`.

### R3. Snapshot section order: spawn records first, and the despawn set grows

A spawn record and deltas for the spawned entities arrive in the same
snapshot; expansion must precede the deltas. The client currently processes
despawns first, then entity blocks (`Replication.cpp:2357-2377`, `2412+`).

**Adopted:** the snapshot gains a **blueprint-spawn section written before the
despawn section**: spawn records → despawns → entity blocks → body states →
events. Client-side, each record is handed to the R4 expander synchronously;
the member deltas later in the same packet then find their entities in
`_entityByNetId`. Two consequences handled:

- **A member dead at spawn time.** The record implies all `memberCount` ids
  (R5), including one the host has since destroyed; the same packet must
  despawn it, so the server's despawn set becomes
  `acked ∪ (ids implied by this packet's spawn records) \ effective` — which
  is also why records precede despawns on the wire.
- **Idempotency.** A record for a `baseNetId` already in the client's map is
  an unacked resend and a no-op.

The unknown-NetId-treated-as-spawn fallback (`Replication.cpp:2442-2456`)
stays as-is: the server only sends member *deltas* once the record is acked
(`known` at `Replication.cpp:1921`), so the fallback is unreachable for
members except through local destruction, where a bare resurrected mirror is
the already-documented behaviour.

### R4. The D5 seam, concretely: two injected functions, not the deferral machinery

The existing deferral (`SetDeferHandshake` / `IsAwaitingLevel()` /
`ConfirmLevelReady()`, `Replication.hpp:1143-1171`) is **once per join and
poll-shaped** — the application notices `IsAwaitingLevel()` across frames and
answers when its world is built (`EditorNet.cpp:477-491`,
`ServerApp.cpp:229-234`). Spawns are continuous and land mid-packet inside a
streaming `BitReader`; a poll-shaped wait cannot suspend `ApplySnapshot`
half-parsed. So the deferral machinery does **not** generalise, and no attempt
is made to force it. The seam is:

**Server:** `ReplicationServer::SetInstanceInfoProvider(InstanceInfoFn)`,
`InstanceInfoFn = std::function<bool(std::uint32_t instanceId, InstanceNetInfo &out)>`,
where `InstanceNetInfo` (declared in NetSync) is
`{ enum Kind { Runtime, Level }; std::uint16_t index; /* content index or level instance index */
std::uint32_t memberCount; Math transform; }`. NetSync reads the tag itself
(typed, via R1) and asks the provider once per newly-seen instance. Installed
by the session owner (NetSession wiring in `EditorPlay`/`ServerApp`), exactly
as §12's `ParentWorldFn` is injected into Physics.

**Client:** `ReplicationClient::SetInstanceExpander(ExpandFn)`,
`ExpandFn = std::function<bool(const InstanceSpawnDesc &, ExpandedInstance &out)>`
with `ExpandedInstance = { std::uint32_t localInstanceId; std::vector<ECS::Entity> members; }`
in flattened member-list order, `NullEntity` for holes. NetSync then, per
replicable member: bind `baseNetId + i`, stamp `Mirrored`
(`Replication.cpp:2448-2455`), route bodies through the `SyncMirrorBody` path
(`Replication.cpp:2604-2638`), bump `StructureRevision`. The App-side
expander runs **mirror-mode expansion**: the ordinary expansion minus
authority physics for `Replicated`-marked members (the join's level-load rule,
applied by the spawn path) and minus inline GPU resolve (the presentation
layer re-resolves on `StructureRevision`, which is how every mirror already
gets its meshes). Systems the blueprint names are queued to `DrainMain`
(stage 8; see R14). Expander failure or an uninstalled expander with a record
on the wire is fatal: `AbortJoin`-shaped teardown with a reason
(`Replication.cpp:2090-2095`), per the brief's "a client that cannot expand
refuses the connection".

### R5. "First snapshot is a delta against the blueprint" — retained mechanism, sharpened claim

Retained from the previous plan (its R3), verified still accurate against
current line numbers. The baseline is a change tick, not a blob: the server
samples `postSpawnTick` immediately after expansion (the same
`_scene.CurrentChangeTick()` sampling `SendSnapshot` does at
`Replication.cpp:1889`), and writing a spawn record into a connection's
`SentSnapshot` (`Replication.hpp:700-716`) implies, for every live implied
member: its id into `netIds`, a `WrittenEntity{id, {postSpawnTick, 0}}` into
`written`, and its **authored replicable component set** (computed from the
prepared form ∩ `_replicatedComponents` minus authored exclusions,
`Replication.hpp:1103-1117`) into `components`. `HandleAck` folds it all in
unchanged (`Replication.cpp:879-922`); the removal diff
(`Replication.cpp:1682-1698`) then correctly reports post-spawn removals —
including a prune, which is a `BlueprintMember` removal and travels by
exactly this path; the keyframe sweep still works because `ResetBaselines`
clears ticks but not acked sets (`Replication.cpp:924-932`).

Sharpened: since expansion writes members before `postSpawnTick`, a member's
components are *never* sent at spawn at all — untouched state costs zero
bytes forever, not just "less". Overrides applied during expansion predate
`postSpawnTick` on both sides identically, which is what D1's second flavour
exists to guarantee. The budget interaction is free: implied entries mean the
priority loop (`Replication.cpp:1891-1966`) has nothing to drain for a
freshly spawned instance until something actually changes.

Contiguous blocks come from the previous plan's R1/R2, retained:
`EnsureInstanceBlock` on the server reserves `memberCount` ids in one bump of
`_nextNetId` the first time either assignment path
(`ReconcileNetIds`, `Replication.cpp:1405-1426`; `EnsureNetId`,
`Replication.cpp:446-466`) meets a tagged entity; `NetId = baseNetId +
memberIndex` over the **file's flattened member list, with holes** for
non-replicated, removed, or already-dead members — free because ids are never
reused (`Replication.cpp:1430-1432`). One care kept: `EnsureNetId` maintains
the live set sorted by appending (`Replication.cpp:463-464`); a late binding
of a reserved id must sorted-insert instead.

### R6. `InstanceDespawn`: one record on the wire, per-entity semantics underneath

The decision keeps the one-record form, and prune replicating (D2) is what
makes it readable. The mechanism adopted: **the despawn section gains
run-length encoding** — `(firstNetId, runLength)` pairs instead of N varints
(`Replication.cpp:1866-1871` writes the set; `2363-2377` reads it). A
`DestroyInstance` produces a contiguous run over the block and goes out as
one pair — the brief's "despawn is one record, not N", literally — while the
semantics stay exactly the existing per-entity set difference:

- resend-until-acked is inherited, no new ack machinery;
- a client holding only some members (mid-join paging, budget cuts) despawns
  exactly what it holds — the diff is computed against *that connection's*
  acked set;
- a member destroyed individually is a run of length 1, and a later
  `DestroyInstance` is a shorter run around the hole;
- prune divergence cannot occur: nothing interprets membership at despawn
  time.

One client-side rule is forced by per-instance relevancy and adopted: **when
the last replicated member of an instance despawns on a client, the client
destroys the instance's remaining (non-replicated) local members and drops
its `baseNetId → instanceId` row** — otherwise a relevancy revoke leaves
ghost fragments and the re-entering spawn record duplicates them. Documented
consequence: a host that individually destroys *every* replicated member of
an instance (without `DestroyInstance`) is indistinguishable from
destruction, so the client's local non-replicated members go too. Those were
never synchronized state; accepted.

### R7. Per-instance server bookkeeping, and the revived-member straggler

Retained from the previous plan (its R4): the instance's wire identity is
`baseNetId`; the server keeps a per-instance net record
`{ instanceId → baseNetId, kind+index (R4), memberCount, postSpawnTick }`
plus a reverse lookup for member→instance. **Amended:** the reverse lookup is
a per-instance `memberIndex → NetId` array, not pure block arithmetic —
because the editor's undo can *revive* a destroyed member mid-session at its
exact old handle (`Scene.hpp:80-95`), `ReconcileNetIds` deliberately picks
revived entities up (`Replication.hpp:534-539`), and its old NetId is retired
and never reused (`Replication.cpp:1430-1445`). Such a straggler gets a fresh
out-of-block id, is recorded in the array, and reaches clients as an ordinary
bare mirror whose tag translates through R2. Relevancy escalation (stage 7d)
uses the array, so stragglers stay glued to their instance. (Whether to allow
this at all is D-B in §3.)

### R8. `ClientHello` gains `contentSetHash`; what bumps and where refusal lives

- `ClientHello` (`NetProtocol.hpp:228-232`) gains `std::uint64_t
  contentSetHash`; `WriteClientHello`/`ReadClientHello` change as a pair.
- This, the spawn-record section, the section reorder (R3), and the despawn
  RLE (R6) are framing changes: **`kNetProtocolVersion` 6 → 7**
  (`NetProtocol.hpp:153`), changelog comment extended as the last three bumps
  were (`NetProtocol.hpp:149-152`). `NetProtocolHash()` folds the version in
  already (`NetProtocol.cpp:52-77`); `NetProtocolSummary()` follows for free.
- The refusal lives in `HandleClientHello` (`Replication.cpp:861-877`),
  beside the protocol-hash check: mismatch sends a new
  `RejectReason::ContentMismatch = 3` (`NetProtocol.hpp:129-133`) with a
  detail string ("content sets differ — remove stray .alvl/.abp files or sync
  assets"; with only a hash the server *cannot* name what differs, which the
  brief allows: "naming what differs if it can"). The client's Reject handler
  currently assumes two reasons (`Replication.cpp:2322-2334`) and is
  extended.
- The content-set hash is **deliberately not** part of `NetProtocolHash()` —
  it is content identity, checked at join time like `LevelIdentity`, not
  protocol identity (the doctrine at `NetProtocol.hpp:135-153`). And
  `LevelIdentity.contentHash` **stays**: it names *which* level and covers
  the PIE temp snapshot, which lives under the user root and is therefore
  outside the content set (`EditorNet.cpp:322-357`,
  `NetProtocol.hpp:176-180`).

### R9. Async hashing: where it rides, and the one ordering trap

The job system fits as-is: `Jobs().Run(Core::Jobs::Pool::Worker, …)` returns
a `Task<std::uint64_t>` polled with `IsComplete()`
(`JobSystem.hpp:117-196`); nothing blocks. Triggered by hosting/joining only
— `StartPlay(Host)` and `StartPlay(Join)` kick it (`EditorPlay.cpp:40-79`),
`ServerApp::OnStart` for the headless host (`ServerApp.cpp:113-135`) — never
by level load, so a single-player game never hashes.

The trap: **`ClientHello` is sent exactly once, at `ConfirmLevelReady()`, and
is never resent** (`Replication.cpp:2068-2088`). A server that received one
while its own hash was pending could neither verify nor safely drop it. So
the wait moves one step earlier on each side:

- **Server:** `ReplicationServer::SetContentSetHash(std::uint64_t)`; until it
  is set, `AddConnection` registers the connection but *withholds the
  `ServerHello`* (today it sends immediately, `Replication.cpp:329-341`,
  `792`); when the hash lands, pending hellos flush. This is the literal
  reading of "the server cannot be reached without one".
- **Client:** the hash becomes a precondition of answering:
  `ConfirmLevelReady(std::uint64_t contentSetHash)`. The applications already
  poll join progress per frame (`EditorNet.cpp:455-492`,
  `ServerApp.cpp:229-234`); they call it when *both* the level is built and
  the task is complete. The join timeout (`kJoinTimeoutSeconds`,
  `EditorNet.cpp:489-491`) becomes phase-aware so a slow first scan does not
  read as a dead host.

The scan itself: recursive enumeration of the asset root for `*.alvl` +
`*.abp` (extension-blind beyond that filter, matching §1's "the extension
never gates behaviour" — both extensions are one set), sorted by virtual
path, each file content-normalised (R10), combined FNV-1a over
`(path, hash)` pairs. Owned by App (`App::BuildContentSetHash`), since it
needs `AssetSystem`. Per-file hash caching in the asset database is deferred
until it is slow enough to matter; the future baked archive replaces the scan
entirely (brief §9).

### R10. Promote the hash helper — it is now a bug fix, not hygiene

`HashLevelFile` exists twice and **disagrees with itself**: the editor folds
CRLF→LF before hashing (`EditorNet.cpp:92-127`, the `562aa5d` lesson) while
`ServerApp`'s hashes raw bytes (`ServerApp.cpp:45-61`) — so an editor host
and a headless client refuse each other over the same CRLF-checked-out level
today. **Adopted:** one `Core::HashTextFileNormalized(path)` beside
`ContentHash64` (`ContentHash.hpp:28`), both existing sites converted, the
content-set scan built on it. Lands first in stage 7a and is worth
cherry-picking out early.

### R11. The instance-aware strip — two sites, not one

The strip is duplicated: `EditorApp::StripReplicatedEntities`
(`EditorNet.cpp:176-225`) and the headless client's inline copy
(`ServerApp.cpp:204-216`). Both walk `Replicated`-carrying entities
per-entity. Both become instance-aware: an entity carrying `BlueprintMember`
escalates to removing its **whole instance** (all members, via the tag scan
`DestroyInstance` uses, plus the instance-table row) so the client's
re-expansion from `BlueprintSpawnFromLevel` cannot duplicate the
non-replicated members. The orphaned-`Parent` cleanup
(`EditorNet.cpp:208-217`) stays and now fires less, since members parented to
members leave together. The loader records each top-level instance's index in
the level file's `instances` array at expansion so both the strip and the
`FromLevel` record agree on identity.

### R12. Instances placed *during* a session use the runtime flavour

`BlueprintSpawnFromLevel` indexes the level file's `instances` array — bytes
both machines verified at join. An instance the host's editor places
mid-session is not in that file, so it travels as
`BlueprintSpawn {blueprintIndex, baseNetId, memberCount, transform}` — which
is correct by tick ordering: a freshly placed instance has no overrides at
placement, and overrides added later are ordinary component writes with
change ticks after `postSpawnTick`, delivered as deltas to current and late
joiners alike. The brief's "placing, moving, overriding, pruning and deleting
instances during a session are all fine" holds, with this one flavour rule
made explicit for the implementer.

### R13. Per-member secrecy is structurally void — presence follows the block

Client-side expansion rebuilds **every** member from the file, so the
authored state of a `Replicated::excluded` or `Relevance::ControllerOnly`
member is on every client's disk and in every client's expansion regardless
of policy. What those policies still control is *updates*. Adopted rule for
relevancy (stage 7d): the block escalation in `ComputeEffective` runs after
the policy filters (`ControllerOnly` erasure at `Replication.cpp:636-650`,
intersection at `653-656`) and re-adds all live members of any touched block;
`ControllerOnly` continues to gate that member's *deltas* via the ordinary
per-entity machinery. The editor may warn that secrecy inside a blueprint is
not a thing. No wire or format change.

### R14. The client-installs-systems bullet completes at stage 8

The brief's own build order ships the format without `systems` until the
`ASYSTEM` catalog exists (`blueprint-system-concept.md`, "Settled while
reviewing → Build order"). Until stage 8 lands, spawn-driven system
installation is vacuous on both host and client (profiles still install at
level load, `World.hpp:83-100`); the expander's install-queueing hook is
added in 7c but wired to the catalog in stage 8. The §9 bullet ("the client
installs the blueprint's systems") is therefore *temporarily* unmet in a
mixed build — acceptable because nothing can author a `systems` list before
stage 8 either.

### R15. The D3 gate site, and what "unsaved blueprint edits" means

The Play/Host flush lands in `StartPlay`'s existing host gates
(`EditorPlay.cpp:40-79`): a new check — any open world whose file belongs to
the content set (any `.alvl`/`.abp`; extension-blind, per §1) and is dirty —
raises a save-or-discard modal in the mould of `DrawHostUnsavedModal`
(`EditorNet.cpp:560-600`). Two amendments to the current code's assumptions:

- The PIE sidestep comment (`EditorPlay.cpp:47-52`) is now only half true:
  the temp snapshot covers *level* edits, not edited-but-unsaved `.abp`
  files a hosted level instances — which is exactly the brief's "green
  handshake over two different worlds" case. The blueprint-flush gate
  therefore applies to **all** Play forms, PIE included, and to plain solo
  Play (the decision says "Play/Host"), which also keeps the prepared-form
  cache coherent with disk.
- Mid-session `.abp` editing stays ungated — undefined behaviour a developer
  opts into, per the decision. No code enforces anything there.

### R16. Textual notes to carry into the brief (no design content)

- §7's "one failure convention, not two" should scope itself to the
  spawn/find-instance calls; `FindMember` returning bare `ECS::Entity` with
  `NullEntity` is the established entity-returning convention.
- §2's "sent once and never again unless a prune changes it" — also resent by
  the keyframe sweep, like all full state. Harmless; say it.
- §9's "twenty-odd bytes" stays directional.

Also retained unchanged from the previous plan, verified still current:
mirrors-not-authority rules (its R10, now folded into R4's mirror-mode
expansion); the `worldComplete` check surviving implied ids (its note —
`std::includes` over acked vs effective, `Replication.cpp:1857-1858`, works
because implied ids enter the acked set on ack).

---

## 3. Inconsistencies — needs a decision

Ranked. None blocks stages 0–6.

### D-A. Does the content-set hash gate PIE? — recommend yes, uniformly

PIE clients share the asset root and the binary with their host, so a
mismatch is impossible in practice, and the scan costs startup latency per
run. Options: (1) hash uniformly — PIE children compute the same hash the
host did, the connection waits on it like any join; (2) exempt PIE
(same-machine assumption), skipping the scan. **Recommendation: (1).** The
scan is async and overlaps the child's own level load, the cost is bounded by
four level files plus whatever blueprints exist for years yet, and (2) is a
second handshake path — the exact thing the strict check exists to avoid.
Revisit only if PIE startup measurably drags.

### D-B. Undo of member create/delete during a live session — recommend allow, with stragglers

The editor's undo can destroy and revive replicated members mid-session
(`Scene::ReviveAt`, `Scene.hpp:80-95`; picked up by design at
`Replication.hpp:534-539`). R7's per-member NetId array absorbs it: a revived
member gets a fresh out-of-block id and clients see a bare mirror. Options:
(1) allow it, ship the array (R7 as written); (2) gate world-structure undo
while a session is active, alongside the existing session-time guards
(`replication-plan-v4.md` §3.6's shape). **Recommendation: (1)** — the
mechanism is needed anyway for late `Replicated`-marking of a member, and (2)
adds an editor-UX carve-out for a case the machinery can simply be correct
about. Flagged because (2) is defensible and cheaper if the array proves
fiddly.

### D-C. Is expansion order protocol surface worth a constant? — recommend yes

Client expansion ≡ host expansion is the delta-baseline premise; an expansion
bugfix that reorders members or changes composed transforms desyncs old-vs-new
builds *without touching any hashed file*. Options: (1) discipline plus a
pinned fixture test; (2) also fold a `kExpansionVersion` constant into
`NetProtocolHash()` (one line beside `kNetProtocolVersion`,
`NetProtocol.cpp:52-77`), bumped whenever expansion semantics change.
**Recommendation: (2)** — it converts a silent cross-build desync into a
refused pairing for the cost of remembering one bump, and the fixture test
(§4, 7b) is the reminder.

---

## 4. The plan

The brief's build order is honored: §12 → §6 → expansion/tag → §5 → §7 verbs
→ §10 editor → §11 prepared form + §9 network → §8 `ASYSTEM` → `InstanceView`.
The format ships without `systems` until stage 8; the editor does not queue
behind codegen. Sizes: **S** ≤ a day, **M** a few days, **L** a week-plus.

### Stage 0 — Physics ignores `Parent` (§12) — S, strictly first

Lift the archived fix (`git show archive/blueprints-attempt-1`:
`PhysicsWorld::ParentWorldFn`) rather than re-derive; Physics links
`Core + ECS + Jolt` only, so the injected-resolver shape is forced.

- **Touches:** `modules/Physics/{include/Assisi/Physics/PhysicsWorld.hpp,
  src/PhysicsWorld.cpp}` (body creation and `InterpolateTransforms`
  writeback); resolver installed from App/world wiring.
- **Format/wire:** none. **Verify:** new
  `modules/Physics/tests/TestParentedBodies.cpp` — a dynamic body under a
  parented entity is created at its composed world pose and the writeback
  decomposes back to the local field; ctest suite `Physics`.

### Stage 1 — Format v2: named entities, named references (§6) — S/M

Smaller than the previous plan's stage 1: **no module extraction** (D5). The
work lands inside `Runtime::SceneSerializer` where the loader already lives.

- **Delivers:** per-entity unique `name` (editor auto-names); `EntityRef`
  fields serialize as names — a fourth mapping mode beside remap/raw/transfer
  (the remap asymmetry documented at `SceneSerializer.hpp:181-184`);
  duplicate-name and unknown-name hard errors; `"version": 2`; the v1
  positional reader **deleted** and the four `assets/levels/*.alvl` converted
  by hand (§6: no migration path).
- **File at this point:** `{version: 2, profile, entities:[{name,
  components}]}` — `profile` survives until stage 8; no `instances` yet.
- **Verify:** `modules/Runtime/tests/TestSceneSerializer.cpp` extended:
  round-trip, duplicate-name refusal, unknown-ref refusal, name→handle
  rewrite; ctest `Runtime`.

### Stage 2 — Expansion core, the tag, the instance table (§§1–4) — L, broken down

- **2a.** `ECS::BlueprintMember` ACOMP (R1) + instance table + per-world id
  allocator (from 1, reset on load) in Runtime (`modules/Runtime`, new
  `Blueprint.hpp/.cpp`); table rows: source path, root transform, and — for
  level-expanded instances — the level `instances` index (R11).
- **2b.** Single-file expansion: create members, apply components, rewrite
  `Parent`/`EntityRef` names to handles, tag, table row; all-or-nothing via a
  staging list.
- **2c.** Nesting: closure walk, cycle hard-fail, flattening with path names
  (`car/body`), transform composition with root evaporation, uniform-scale
  hard-fail at every level naming file+instance.
- **2d.** App composition: `LevelRuntime` gains the instances path —
  expansion + asset resolve + physics build for `LoadLevel`, and the
  render-free half for `LoadLevelSim` (`LevelRuntime.hpp:118-136`), which is
  what keeps the headless server working with zero link changes (D5's
  premise: `apps/sandbox/CMakeLists.txt:14-23` already links everything).
- **File:** adds `"instances": [{name, source, transform}]`; no overrides
  yet. **Wire:** none.
- **Verify:** new `modules/Runtime/tests/TestBlueprintExpansion.cpp`
  (flattening, composition, cycle refusal, scale refusal, staging rollback on
  a missing nested file); an App-level headless load test in
  `modules/App/tests`.

### Stage 3 — Overrides (§5) — M

The merge lattice: outermost-wins-per-field between object claims; removal
(`null`) beats an outer field override with a warning naming
level/member/component; adds start from C++ defaults; two adds merge
per-field; `removed` as downward paths; orphaned overrides dropped with a
log; overrides naming undeclared members hard-fail.

- **Touches:** Runtime expansion. **File:** instance entries gain
  `"overrides"`/`"removed"` — the final pre-`systems` format.
- **Verify:** `TestBlueprintOverrides.cpp` — the three-level
  `RigidBody: null`-then-`{mass: 5}` case verbatim; add-after-remove
  defaults; removal of a since-deleted component no-ops; the subset
  invariant.

### Stage 4 — The four verbs and queries (§7) — S/M

`SpawnBlueprint(world, source, transform) → optional<uint32_t>`;
`DestroyInstance` (tag-pool scan; `Scene::Destroy` deferral already safe);
`PruneFromInstance`; `ExplodeInstance`; `FindMember`; untyped `FindInstance`
with the table's source check. App wiring so a runtime spawn resolves assets
and builds physics (same composition as 2d).

- **Verify:** `TestBlueprintVerbs.cpp` — destroy reaches only tagged members
  and spares the loose neighbour; prune-then-destroy spares the pruned
  entity; explode; spawn failure leaves nothing; the id outlives member
  death, handles do not.

### Stage 5 — Editor (§10) — L, broken down; parallel with 4/6/7a after 3

- **5a.** `InstanceDelta` third variant on `EditCommand`
  (`EditHistory.hpp:85`); every member edit transacts the `ComponentDelta`
  with the record mutation; the per-gesture diff *is* the recording (brief,
  "Settled while reviewing").
- **5b.** Outliner instance rows + two-mode selection; picker shows
  `car_3 › wheel_fl`; inspector header, override marks, per-field/component
  resets.
- **5c.** Gizmo Instance frame: the world/local toggle at
  `EditorGizmo.cpp:121-123` becomes three-way; the instance transform folds
  into the view matrix, overrides recorded in file space either way (§3).
- **5d.** Re-expansion as in-place diff; history truncation below the oldest
  transaction naming a deleted member; the transient-rebuild invocation is
  new work (the rebind hook function is shared, its invocation is not).
- **5e.** The R15 gate: Play/Host flushes dirty content-set worlds
  (`EditorPlay.cpp:40-79`, modal per `EditorNet.cpp:560-600`).
- **Verify:** `modules/Editor/tests` — undo/redo of place/prune/member-edit
  restores scene *and* record; re-expansion preserves surviving members'
  exact `(slot, generation)`; truncation leaves a replayable stack. By eye:
  drag a car, save, confirm zero member overrides in the file.

### Stage 6 — Prepared form (§11) — M; after 4, ideally before 7b

One `BinaryCodec` block per member plus a `FieldType::EntityRef` fixup table;
cache keyed by virtual path in Runtime; warmed by closure walks, cleared on
level unload, invalidated by editor re-expansion. Spawning becomes
create-N/decode/patch. 7b reads it for authored component sets (R5) but can
interim-parse JSON.

- **Verify:** `TestBlueprintPreparedForm.cpp` — cache-spawn ≡ JSON-spawn
  field-for-field including an `AssetIdVector` field (no shared
  allocations); second spawn parses nothing.

### Stage 7 — The network (§9) — L, broken down

**7a. Content-set hash and the handshake — M. Can start after stage 0; only
the refusal test needs stage 1's files.** Promote the hash helper (R10);
`App::BuildContentSetHash` scan (R9); `ClientHello.contentSetHash`,
`RejectReason::ContentMismatch`, withheld-`ServerHello` server gate,
`ConfirmLevelReady(hash)` client gate, phase-aware join timeout (R8/R9);
**bump `kNetProtocolVersion` 6→7** — batched with 7b's framing changes if
they land together, per the `NetProtocol.hpp:135-153` doctrine.
- *Verify:* `modules/Core/tests` — CRLF file ≡ LF file;
  `modules/NetSync/tests/TestNetSession.cpp` — mismatched set refuses with
  the new reason, matching set joins, a client connecting before the server
  hash is ready receives no hello until it lands, a hash-pending client
  neither sends a hello nor times out spuriously; a reordered-enumeration
  test pinning sort-by-path determinism.

**7b. Server half — L.** `AFIELD(instanceRef)` + codec hooks + layout-
description fold (R2 — touches `tools/reflectgen`, `Core/Reflect/FieldMeta.hpp`,
`Core/src/BinaryCodec.cpp`, with fuzz cases in `TestBinaryCodec.cpp`);
`EnsureInstanceBlock` reached from both id-assignment paths (R5/R7);
per-instance net records + `SetInstanceInfoProvider` (R4); spawn-record
emission in `SendSnapshot` when the first member of an instance unknown to
the connection drains from the priority loop, implying
`netIds`/`written{postSpawnTick}`/authored component sets (R5); section
order records→despawns→blocks and the extended despawn set (R3); despawn RLE
(R6); `kExpansionVersion` if D-C lands. `HandleAck` needs no change.
- *Verify:* `TestBlueprintReplication.cpp` (server driven directly, the
  `TestReplication.cpp` style): record once per connection, resent until
  acked; block contiguity under interleaved ordinary spawns and same-tick
  `EnsureNetId` event sends; zero member bytes after spawn until a change;
  post-spawn change arrives as the only delta; post-spawn removal and prune
  arrive via the removal diff; member dead-at-spawn despawned in the same
  packet; keyframe sweep resends full state but never the record; a pinned
  expansion-order fixture (D-C).

**7c. Client half — M.** `SetInstanceExpander` + mirror-mode expansion in App
(R4); binding, `Mirrored`, `SyncMirrorBody`, no authority physics for marked
members; idempotent resend handling (R3); `baseNetId → instanceId` map +
`instanceFromWire`; the R6 teardown rule; install-queue hook stub (R14);
expansion failure = fatal teardown with reason.
- *Verify:* loopback client/server: convergence of entity count and field
  state against a host that spawned, mutated one member, pruned another,
  destroyed a third; the client finds the blueprint's non-replicated
  component locally (§9's third bullet as a test); a bad nested source (hash
  check bypassed in-test) tears down.

**7d. Per-instance relevancy — M.** Block escalation in `ComputeEffective`
after the policy filters and intersection (`Replication.cpp:636-656`, R13);
instance-granular re-entry escalation in the `668-675` rule and `ForgetAcked`
(`712-750`); per-connection known-instance vector maintained by the same
in-flight/ack discipline; stragglers glued via R7's array.
- *Verify:* extend `TestRelevancy.cpp`/`TestDistanceRelevancy.cpp`: one
  wheel named pulls the whole car; leave-and-re-enter within one round trip
  resends the record and full member state; revoke despawns all members as
  one RLE run and the client drops row + local members; `worldComplete`
  still completes for a filtered joiner.

**7e. `BlueprintSpawnFromLevel` + instance-aware strip — M.** The second
record flavour reading the loader-recorded instance index (R11/2a); both
strip sites become instance-aware (`EditorNet.cpp:176-225`,
`ServerApp.cpp:204-216`); editor-placed-mid-session instances take the
runtime flavour (R12). Provisional by design — deleted when
`replication-plan-v4.md` §5's shared-baseline join lands.
- *Verify:* end-to-end join with a level instance overriding one member
  field and removing another: client equals host including the override,
  with zero component blocks for untouched members (byte-level assert, the
  identity-filter test's style, `Replication.hpp:828-835`); no duplicated
  non-replicated members after join; a mixed level (loose replicated
  entities + instances) survives the strip unregressed.

### Stage 8 — `ASYSTEM` and the catalog (§8) — L; parallel from stage 2 onward

Unchanged from the previous plan, still valid: reflectgen grammar
(`tools/reflectgen/reflectgen.py`; the whole-tree check reuses the
`--check-handlers` shape, `reflectgen.py:303`); registration rides the OBJECT
library (`cmake/AssisiReflect.cmake:82-84`); catalog + deferred install at
`DrainMain` (`Application.cpp:509-518`); delete `registerGameSystems` and the
profile's `SetContactReporting` misplacement (`apps/sandbox/src/main.cpp:274`);
**second file conversion** — `systems` joins the format, `profile` leaves,
`World`'s profile plumbing retires (`World.hpp:83-100`); wire the 7c/host
install-queue hooks to the catalog (R14).
- *Verify:* reflectgen tests (duplicate names, missing `after`/`before`
  targets, cycles fail the build); spawn inside a system installs at the next
  safe point and runs the following frame; double-naming installs once;
  unknown name hard-fails the load; a joined client runs a blueprint-named
  system it never had installed before the spawn.

### Stage 9 — `InstanceView<T>` codegen (§7) — M, last

Unchanged: explicit opt-in list read by the build (reflectgen source-list
shape); undefined primary template (`MessageTraits<T>` idiom,
`MessageMeta.hpp:119`); nested grouping; reflectgen ban on views inside
components; move-only, `[[nodiscard]]`.
- *Verify:* compile-fail on a member typo; the `car_body` vs `car/body`
  collision generates distinct fields; a typed-spawn runtime test.

### Order and parallelism

Strictly sequential spine: **0 → 1 → 2 → 3 → 4 → 7b → 7c → 7d → 7e**.
Parallel: **7a** after stage 0 (fully testable after 1); **5** after 3
(5c after 2); **8** after 2; **6** after 4 (before 7b preferred); **9** after
4 + 8's build machinery. The two file conversions (stages 1 and 8) are the
flag days; do each on a quiet branch point.

---

## 5. Risks

1. **Every expansion code path is now protocol surface.** Same file, same
   hash, different member order or composed transform ⇒ silent cross-build
   desync. Mitigations: the pinned expansion fixture in 7b, one shared
   composition function both sides call, and D-C's `kExpansionVersion` if
   adopted.
2. **Float determinism of composed transforms across platforms.** Unchanged
   components are never resent, so a static member can sit ULPs off
   permanently; the keyframe sweep (`Replication.hpp:126-142`, on by
   default) bounds it to ~8.5 s and bodied members are corrected anyway.
   Keep the composition in one function; let neither side "optimize" it.
3. **7b touches the most invariant-laden file in the tree.** Implied
   `SentSnapshot` entries interact with the budget-skip baseline rule
   (`Replication.hpp:674-687`), the in-flight scrub in `ForgetAcked`
   (`Replication.cpp:729-749`), and the ack fold (`879-922`). Land the new
   tests *before* touching the sections; run `TestReplication`,
   `TestRelevancy`, `TestBodyReplication` per commit.
4. **The codec change (R2) reaches Core's trust boundary.** `ReadComponent`
   runs on hostile bytes (`BinaryCodec.hpp:32-36`); the `instanceRef` hook
   must inherit the never-reads-outside-the-buffer discipline and gets fuzz
   cases in `TestBinaryCodec.cpp` alongside the existing ones.
5. **The strip change (7e) reaches the recently-landed join flow twice**
   (editor and headless, R11), each with its own physics/`Parent` cleanup
   invariants. The mixed-level join fixture is the guard; the duplication
   itself argues for extracting one shared strip helper while in there.
6. **Withheld `ServerHello` (R9) is a new handshake state.** A hash job that
   never completes (unreadable file mid-scan) must fail the *task* loudly and
   tear down hosting, or connections hang in limbo the join timeout then
   misattributes. The 7a tests cover the pending-hash paths on both sides.
7. **Two file-format conversions with no migration path** (stages 1 and 8) —
   deliberate (§6), each a flag day for uncommitted level work.
8. **The R15 gate is easy to ship without.** Nothing fails when PIE hosts an
   unsaved `.abp` — hashes match and worlds diverge, the exact failure §9's
   check exists to prevent. Land 5e in the same commit as 7a so the hash
   never ships without the flush.
