# Replication Plan v4 — Local Simulation with Authoritative Correction

> **Build status, 2026-08-01: R1–R8 implemented and committed on `networking`;
> R9 partly done.** Everything with a library-level definition of done is green
> — 73 NetSync cases, 13/13 ctest targets, optimized build clean — and the
> headless half of R9's matrix has been run over real UDP. What has *not* been
> checked is everything whose DoD says "by eye" or needs two editor windows;
> that list is §4a below, and it is the honest remainder of this plan.

Status: **planned, not started**. This replaces `replication-authoring-plan.md`
(v3.5), which stays in the tree as the historical record. v4 is written from
scratch rather than amended into v3.5 because two of that plan's foundations
were overturned by decision — mirrors are no longer passive, and joining is no
longer an Editing-state exception — and a plan whose §2 exists to govern a mode
that no longer exists is cheaper to replace than to renovate. Revised twice:
once after an adversarial correctness review (session-scoped world-structure
guards, the client-write rule, the R4 split, the sweep ack-race and
baseline-retirement details, correction-application details, two citation
corrections), and once after a usability review that judged the design as if
already shipped (the host-time save modal, host/join on one surface, the
priority floor, resurrection telemetry, the trap legend, and smaller
affordances noted in place).

**Branch: `networking`.** This plan builds on the existing implementation and
was written for that branch deliberately, not by default. The pre-approved
alternative — a clean `networking_v2` off `dev` — was considered and rejected
after reading the code: of the ~5,100 lines on this branch, the transport, the
bit codec, the protocol hash, the input-command path, the net clock, the
headless Application, and the session shell are orthogonal to the replication
*model* and would be rebuilt line-for-line-equivalent on a fresh branch, minus
their 49 green test cases and the GNS/protobuf CMake integration that took a
pinned-SHA fight to earn. The one file genuinely shaped by the old model —
`modules/NetSync/src/Replication.cpp` — turns out to be about half right for
the new one: the server's acked-baseline delta machinery survives (§3.4
explains why it is *more* load-bearing under the new model, not less), and what
gets rewritten is the client's view half and the server's capture source. §3.2
draws the exact keep/rewrite line. Scrapping the branch would discard the
chassis to avoid rewriting one wheel.

---

## 0. What v4 is, in a paragraph

The server owns *what is true*; clients simulate *what happens next*; the
server's job on the wire is to periodically **re-anchor** clients to the truth,
not to stream it continuously. Concretely: every machine runs the same binary
and steps the same Jolt world at 60 Hz — replicated physics entities have real
dynamic bodies on clients, not kinematic ghosts — and the server sends
authoritative body state (pose, velocities, sleep state) for entities that are
awake or just changed, against the per-connection acked baseline that already
exists. A correction snaps the client's *simulation* to the server's value and
hides the jump with a decaying *visual* offset, so the simulation is always
honest and the screen is always smooth. Everything else in the system — what
an entity is, which components it has, what it looks like — keeps flowing
through the existing reflection-driven delta-snapshot path, which was built,
tested, and is not the part that was wrong. The one rule the rest falls out
of: **the wire carries corrections to a simulation both sides are running,
except where only one side runs it — and then it carries the state itself.**

This is the model the literature calls state synchronization
([Gaffer On Games, "State Synchronization"](https://gafferongames.com/post/state_synchronization/)):
run the simulation on both sides, send per-object position, orientation,
linear and angular velocity, apply on arrival, smooth the error. It is also
the shape of Unreal's default physics replication, which corrects "by altering
each object's velocity on the client to match the object's velocity on the
server"
([Networked Physics Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/networked-physics-overview)),
and the non-rollback half of what Rocket League ships
([GDC 2018, "It IS Rocket Science!"](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf)).

## 1. Why the last two plans failed, briefly

**v2 built layers and shipped no feature.** Transport, codec, clock, and
replication core all landed green at the library level — two `Scene`s converge
across a lossy loopback in the test suite to this day — and the first live
two-editor test replicated nothing, because no milestone ever ended at "a
human watches it work." Three integration gaps did the damage: nothing ever
marks an entity `Replicated`; join never tells the client which level to load;
mirrored entities never resolve their mesh GUIDs and draw nothing. All three
still stand and are closed by R1 and R2 below.

**v3.5 fixed the milestone shape and drowned in incidental rules.** Its §2
alone carries roughly ten interacting mechanisms — handshake ordering, level
identity, content hashing, unconditional disk load, strip semantics, orphan
handling, save guards, play guards, disconnect reload, structure-dirty resolve
— most of them real problems that each got bespoke machinery because the plan
put a joined client in a state the editor does not otherwise have (an Editing
scene fed by a session). v4's two upstream decisions dissolve most of that
section: sessions are Play-bound on both sides, and the editor's existing play
snapshot/restore already provides, for free, almost everything §2 built by
hand (§3.6 lists exactly what collapsed and what genuinely did not).

v3.5 also carried a model decision this plan reverses: mirrors were read-only
ghosts with no Jolt bodies, rendered two snapshot intervals in the past. That
delay is structural — the client interpolates between snapshots that have
already arrived, so no bandwidth or rate change removes it; there is a test
pinning it at exactly two intervals
(`TestReplication.cpp`, *"the interpolation delay is two snapshot intervals of
the server's rate"*). Local simulation is the decision that removes it, and it
is settled input to this plan, not something it argues for.

## 2. Grounding: what the code actually does today

Everything in this section was verified against the source on `networking` at
the time of writing; where the older docs disagree with it, the code wins.

- **The delta core is acked-baseline, and self-heals delivery.** Each
  connection remembers the entity set, component set, and scene change tick of
  the last snapshot the client *acknowledged*
  (`Replication.hpp:161-181`). Despawns are the set difference between acked
  and live (`Replication.cpp:406-411`); component removals the same one level
  down (`Replication.cpp:344-358`); state is included when
  `ChangedById(entity, id, ackedChangeTick)` says so, with baseline 0 meaning
  "send everything" — spawn, late join, and keyframe are one code path
  (`Replication.cpp:374-377`). Because the baseline only advances on ack, a
  *lost* snapshot costs bandwidth, never correctness: anything undelivered is
  still "changed since baseline" next tick and goes again. This property is
  the single most important thing v4 inherits, and §3.4 leans on it hard.

- **The client half is passive.** Mirrors are created bare, receive components
  through the reflection codec, and render from a three-sample transform
  history interpolated two snapshot intervals in the past with hold-last-pose
  at the edges (`Replication.cpp:805-892`). No Jolt bodies, no local motion.
  This is the half v4 rewrites.

- **Everything serializable travels.** `_replicatedComponents` is every
  serializable reflected component except the `Replicated` marker itself
  (`Replication.cpp:94-100`). There is no per-type or per-field gate;
  reflectgen understands only `tracked` and `transient`
  (`tools/reflectgen/reflect_codegen.py`). A marked entity therefore ships its
  `Camera` (whose `isActive` could hijack the client's view), and would ship
  any future gameplay-local component by default.

- **Untracked components never delta.** `ChangeTickById` returns 0 for a pool
  without a tick lane (`Scene.hpp:250-255`), which reads as "unchanged", so
  untracked types — `MeshRenderer` and `Name` among them — transmit at spawn
  and then never again, no matter how they change. A live bug today, fixed by
  R1's `replicated`-implies-`tracked`.

- **Sleeping bodies never stop replicating on a windowed host.** The physics
  writeback `PhysicsWorld::InterpolateTransforms` runs every render frame over
  every dynamic body and stamps the Transform's change tick through a
  `QueryMut` proxy — deliberately, so motion replicates at all
  (`PhysicsWorld.cpp:694-747`). But it does not skip sleeping bodies: a body
  asleep for an hour is still stamped every frame, still "changed", and still
  ships its full Transform every snapshot. The celebrated *"a moved entity
  converges, and an unmoved one stops costing bandwidth"* test passes only
  because its harness moves entities by hand and has no `PhysicsWorld`. On a
  real physics level the idle-bandwidth property does not exist today.

- **A headless host replicates stale poses.** The writeback lives in the
  render path; a headless host never runs it, so physics-driven entities'
  Transforms — which is what replication reads — sit at their load pose
  forever. The sandbox host demo works because it writes Transforms directly
  (`ServerApp.cpp:122-128`). Both this and the previous point share one root
  cause: **replication reads the render-side Transform instead of the
  physics-side truth**, and v4's body-state capture (§3.3) removes that
  coupling instead of patching it twice.

- **A latent staleness bug in the budget path.** When a snapshot hits the byte
  budget, an already-known entity is skipped but still recorded in the
  in-flight snapshot, whose *global* `sceneChangeTick` becomes the
  connection's baseline when acked (`Replication.cpp:417-419`, `430-448`,
  `223-230`). The skipped entity's pending changes are then older than the
  baseline and are never resent: an entity whose final change lands in a
  budget-starved snapshot stays stale until something re-stamps it. Today the
  budget rarely binds, and a continuously-moving entity re-stamps itself every
  tick, which is why nothing has noticed; under v4's keyframe sweeps and
  priority-budgeted corrections the budget binds by design, so R4 replaces the
  per-connection global baseline tick with per-entity baselines (§3.4). Worth
  a regression test regardless of everything else in this plan — it is a
  correctness bug in the shipped core.

- **Component removal is handled.** The ECS-level gap is real —
  `Scene::Remove` stamps nothing — but the replication layer routes around it
  with the acked component-set diff, and three component-removal test cases
  (plus a marker-removal-as-despawn sibling) pin it. No v4 work; noted so
  nobody re-solves it, and flagged as a caveat for any *future* change-tick
  consumer that is not the replication layer.

- **Quantization is built, tested, and unused.** `WriteFloatQuantized` /
  `ReadFloatQuantized` round-to-nearest over a bounded range
  (`BitStream.cpp:124-139`, `307-317`); zero callers outside their tests.
  Every field encoder is whole-value. §3.7 and R8 place this deliberately.

- **The editor's session is not Play-bound.** Host/Join live on a panel usable
  in any state; the net pump comment at `EditorApp.cpp:663-669` still defends
  editing-while-hosting, a mode the decisions have since removed; `StartPlay`/
  `StopPlay` (`EditorPlay.cpp:38-171`) know nothing about sessions. Meanwhile
  the play machinery already provides exact-identity snapshot/restore via
  `Scene::ReviveAt`, and Save/Open are already disabled outside Editing
  (`EditorLevels.cpp:66,87,97`) — which is precisely the machinery §3.6
  collapses v3.5's §2 into.

## 3. The design

A note on the "(trap N)" tags below: they refer to the commissioning brief's
numbered list of known traps, which is not in the tree — so the ones cited
here get one line each. **Trap 1**: sleeping bodies versus change-tick deltas
— a body that sleeps stops being "changed" and could stay silently wrong
forever. **Trap 2**: determinism is not available and must not be assumed —
fast-math is on in dev and ship. **Trap 3**: divergence is not uniform —
contact-rich piles amplify error chaotically, ballistic bodies drift gently.
**Trap 4**: correction visibility — teleporting to the authoritative pose
pops. **Trap 5**: authority — server authority must survive client
simulation. **Trap 8**: v3.5's incidental complexity — look for collapses
into existing machinery before inventing new. The brief's remaining traps
(quantization unused, v2's no-watchable-feature failure, the three
integration gaps, invisible component removal, the Windows build) are
addressed where they arise, untagged.

### 3.1 The model, and why this exact shape

Four architectures were on the table; the constraints pick one.

1. **Deterministic lockstep** — rejected long ago and stays rejected:
   `-ffast-math` is live in dev and ship (`CMakeLists.txt:184-207`, whose
   comment records that the networking design assumes it stays on), and Jolt
   is built without cross-platform determinism. Not revisited.
2. **Snapshot interpolation** (v2/v3.5's model) — works, is built, and
   structurally renders the past. Removed by decision, kept for the one class
   of entity it is still right for (§3.5).
3. **Full predict-everything with rollback** — Rocket League's model: the
   client simulates all physics ahead of the server, and on a mismatched
   correction it *"revert[s] all physics actors to that frame in history"* and
   *"run[s] multiple physics frames to catch up"*; the slides price it
   honestly — *"expensive corrections: 200ms ping, 120hz = 24 correction
   frames"* — i.e. a multi-step whole-world resimulation per correction
   ([Cone, GDC 2018](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf)).
   Unreal's Resimulation mode is the same idea with the same cost profile
   ([Networked Physics Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/networked-physics-overview)),
   Unity's Netcode for Entities moves physics inside the prediction loop
   and re-simulates from every received snapshot
   ([Intro to prediction](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/intro-to-prediction.html)),
   and the third-party Unity stacks productize the same shape — FishNet frames
   client-side prediction as "a form of server authoritative movement" with
   reconciliation
   ([FishNet docs](https://fish-networking.gitbook.io/docs/guides/features/prediction/what-is-client-side-prediction)).
   Without near-determinism, predictions are always slightly wrong, so this
   buys its latency with a permanent resimulation tax — a 2-5× physics-cost
   multiplier at our tick rates. Deferred, not rejected: it is the correct
   *future* model for the player's own pawn, where responsiveness justifies
   the cost, and it layers onto v4's protocol without a wire change (§5).
4. **Local simulation with authoritative correction** — this plan. Both sides
   simulate; the server sends state for what changed; the client applies and
   smooths. No rollback, no resimulation, no determinism assumption: the
   correction stream *is* the mechanism that bounds non-deterministic drift,
   which is why fast-math is a non-issue here by construction.

One honest nuance on determinism, so it cannot be re-litigated on a false
premise: Jolt *is* deterministic for "the same binary code … it doesn't matter
if you have an AMD or Intel processor," provided every simulation-mutating API
is called in exactly the same order
([Jolt, Deterministic Simulation](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)),
and the target case here is same-binary. That does *not* make the client's
mirror sim converge on its own: a joining client starts from state that
crossed the wire (not the server's bit-exact state), applies corrections the
server never applies, and adds/removes bodies on a different schedule — the
"same API calls in the same order" condition fails immediately and forever.
Determinism would only help under full input-replication lockstep, which is
architecture 1. So the correction interval must be justified by *measured
drift*, never by an appeal to same-binary determinism — and the panel
telemetry in R6 exists to produce exactly that measurement. The drift itself
is real and fast in the contact-rich case: even a simulation fed *identical
inputs* visibly desynchronizes when any source of reordering exists, and
"after the smallest divergence the simulation gets further and further out of
sync" ([Gaffer, "Deterministic Lockstep"](https://gafferongames.com/post/deterministic_lockstep/)).

**Timelines.** The mirrored physics world runs at *server time minus one-way
transit*: the client applies each correction when it arrives and simulates
forward at 60 Hz in between, so its timeline sits a transit-time behind the
server's, consistently. This removes the structural two-snapshot-interval
delay (100 ms at the 20 Hz default) plus the stepping between snapshots; what
remains is the speed of light plus smoothing. It is *not* "the present" in the
strict sense — rendering the server's present requires prediction ahead of
received state, which is architecture 3 and deferred with the pawn. Meanwhile
input keeps its own, opposite lead: the existing `NetClock` runs the client's
*command* stream ahead of the server by ½·RTT plus buffer (the Overwatch
scheme, ~96 ms of lead at 160 ms RTT, with time dilation deferred —
[Edgegap's Overwatch deep-dive](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback)).
Two timelines in one process is normal in this class of design — Unity's
predicted/interpolated split is the productized form
([Intro to prediction](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/intro-to-prediction.html)).

**Authority (trap 5).** In v4 there is *no client→server state channel at
all*. Clients send `InputCommand`s — rate-limited, clamped, tick-bounded, all
already built — and nothing else; everything a client simulates is
presentation-side extrapolation that the next correction overwrites. A client
cannot push a crate through the wire because the wire has no verb for it. When
the player pawn arrives (deferred), the client's push of a shared object
remains a *prediction*: the server's own contact resolution does the real
push, exactly as Halo phrased it — the client asks, the host simulates the
result ([Halo: Reach networking, GDC 2011 summary](https://www.wolfire.com/blog/2011/03/GDC-Session-Summary-Halo-networking)).
Server authority survives v4 because v4 never grants the client any.

### 3.2 What survives, what is rewritten — the line

**Survives untouched** (no design change, no rewrite):
- `modules/Net` — transport, lanes, loopback pairs, simulated conditions.
- `Core::BitStream` — including the dormant quantizers R8 finally calls.
- `Core::Reflect::BinaryCodec` — component blocks, field masks, protocol
  hash/summary; R1 *extends* its inputs, R5 adds a sibling section.
- `NetClock`, `InputCommand`/buffer/queue, input hardening.
- Headless `Application`, `LoadLevelSim`, the pacing loop.
- `QueryMut` and the change-tick substrate.
- The handshake/reject flow, ack transport, in-flight snapshot ring.
- The GNS pin, the protobuf/abseil build, the test fixtures and soak harness.

**Extended** (same design, more of it):
- reflectgen: `replicated` / `norep` annotations (R1).
- `NetProtocol`: level identity in `ServerHello` (R2), the body-state snapshot
  section (R5), a `RequestKeyframe` control message (R6), a protocol version
  constant folded into the hash.
- `ReplicationServer`: per-entity baselines replacing the global
  `ackedChangeTick` (R4, also the latent-bug fix), body-state capture and
  send (R5), the priority accumulator finally using `Replicated::priority`
  (R6).
- `PhysicsWorld`: four small pimpl passthroughs (§3.3).
- The editor Network panel: telemetry (R6), warnings and authoring UX (R7).

**Rewritten**:
- `ReplicationClient`'s view half *for bodied entities*: the interpolation
  buffer is replaced by body construction, correction application, sleep
  enforcement, and error smoothing. Non-bodied entities keep the existing
  interpolation path unchanged, tests and all (§3.5).
- The editor's session lifecycle: panel-driven join-any-time becomes
  Play-bound join/host (R2), and the `EditorApp.cpp:663` pump comment is
  rewritten to match the design that actually exists.

**Discarded from v3.5**:
- The session-viewer mode and every rule that existed to govern it (§3.6).
- The `NetProvider` seam (§5 — deferred with the reason).
- "Mirrors are visual; do not replicate `RigidBodyDescriptor`" — reversed:
  the descriptor is exactly what the client needs to build its local body.

### 3.3 Body state: capture, wire, apply

**The state, and why exactly this state.** A correction that places a body
but not its motion re-diverges within a frame — "state synchronization runs
the simulation on both sides, so it's always extrapolating from the last
state update applied to each object," which is why velocities are part of the
state, with an at-rest bit making the common case cheap
([Gaffer, "State Synchronization"](https://gafferongames.com/post/state_synchronization/)).

```cpp
// modules/NetSync/include/Assisi/NetSync/BodyState.hpp (new)
struct BodyState
{
    NetId     netId = InvalidNetId;
    glm::vec3 position{};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 linearVelocity{};        // omitted on the wire when asleep
    glm::vec3 angularVelocity{};       // ditto
    bool      asleep = false;
};
```

**Capture (server, once per tick, replacing Transform-as-motion-source).**
NetSync gains a link on `Assisi::Physics` — which is Core+ECS+Jolt and
render-free, so the headless link stays clean; the layering rule being
protected was "never depend on Runtime," and Physics is the module NetSync was
told to imitate, not one it must avoid. (The alternative — an App-side bridge
feeding plain structs both ways — was considered and rejected: it would move
half the protocol's semantics out of the module whose header declares that
the two halves "must be read together".) Both halves take an optional
`Physics::PhysicsWorld *`; null preserves today's behavior exactly, which
keeps all 45 existing NetSync cases meaningful. The constructors and their
call sites:

```cpp
ReplicationServer(Net::NetTransport &transport, ECS::Scene &scene,
                  Physics::PhysicsWorld *physics = nullptr, ReplicationConfig config = {});
ReplicationClient(Net::NetTransport &transport, ECS::Scene &scene, Net::ConnectionId connection,
                  Physics::PhysicsWorld *physics = nullptr);
NetSession(ECS::Scene &scene, Physics::PhysicsWorld *physics = nullptr,
           ReplicationConfig config = {});
// Call sites: the editor passes its play world's physics (EditorNet.cpp:89,105
// construct NetSession(*_scene) today); ServerApp passes &_physics
// (ServerApp.cpp:80); every existing test passes nothing and is unchanged.
```

Per tick, before snapshots are built:
- Ask the world for every active body's state. The order Jolt returns active
  bodies in is unspecified (its own header carries thread-safety caveats on
  the accessor —
  [`GetActiveBodiesUnsafe`, PhysicsSystem.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/PhysicsSystem.h))
  — which is fine, because everything here is re-sorted by NetId.
- For each active replicated body: record its state and bump its **body-state
  tick** from the scene's change-tick counter.
- For each body active last tick but not this one: record its *rest* state
  with `asleep = true` and bump its tick once more — the sleep transition is
  itself a change, and the one whose loss used to be fatal.
- A freshly assigned NetId initializes its body-state tick to "dirty", so a
  world that settled *before* anyone joined still ships its rest poses —
  joining a finished pile must produce sleeping mirrors at the server's exact
  rest poses, not a client-side re-settle.

The four `PhysicsWorld` additions, all thin pimpl passthroughs in the style of
what is already there:

```cpp
struct ActiveBodyState
{
    ECS::Entity entity;
    glm::vec3   position;
    glm::quat   rotation;
    glm::vec3   linearVelocity;
    glm::vec3   angularVelocity;
};
void GetActiveBodyStates(std::vector<ActiveBodyState> &out) const;
bool IsBodyActive(const RigidBody &body) const;
void DeactivateBody(const RigidBody &body);
// One call for pose + velocities + activation, because the existing pieces
// have wrong semantics for corrections three times over: SetBodyTransform
// reactivates unconditionally (PhysicsWorld.hpp:199) — an "asleep" correction
// applied through it would wake the body — and it zeroes velocities
// (PhysicsWorld.cpp:800-804), and there is no angular-velocity setter at all.
// Uses Jolt's EActivation::DontActivate when activate is false. Must also
// collapse both render-interpolation snapshots onto the target, exactly as
// SetBodyTransform already does (PhysicsWorld.cpp:805-812): §3.5's smoothing
// math assumes the rendered pose is unchanged at the instant of a correction,
// and if the writeback smears the jump over a frame while the visual offset
// also absorbs it, the two double-count into a wobble at every correction.
void ApplyBodyState(const RigidBody &body, glm::vec3 position, glm::quat rotation,
                    glm::vec3 linearVelocity, glm::vec3 angularVelocity, bool activate);
```

**Wire.** The snapshot gains one section after the entity blocks; framing
version bumps the protocol constant that feeds `ProtocolHash`, so mismatched
builds refuse at handshake as they do today:

```
[SnapshotHeader]
[despawns: count varint, netId varint...]
[entity component blocks ... (bool-chained, as today)]
[body states: count varint, then per body:]
    [netId varint][asleep 1 bit]
    [position 3 × float32][rotation 4 × float32]
    [if !asleep: linearVelocity 3 × float32][angularVelocity 3 × float32]
```

Whole-value floats in R5; R8 swaps the field encoders for the quantized forms
without touching this structure (§3.7). An awake record is ~54 bytes, an
asleep one ~30. Selection is baseline-driven like everything else: a body is
included when its body-state tick is newer than *this connection's baseline
for this entity* (§3.4), drained in priority order to the byte budget — with
one gate: a body state is only included for an entity the connection already
has (in its acked set) **or whose entity block was written into this same
snapshot**. Without the gate, a new entity whose spawn block was budget-cut
would ship a body state the client has no mirror or descriptor to apply to.
The client, for its part, skips (parses and discards) a body record whose
NetId it does not know — a benign race under loss, not an error.

Two consequences worth stating because they delete existing behavior:

- **Transform component blocks stop carrying motion for bodied entities.**
  For an entity with a rigid body, the Transform block is included only on the
  empty baseline (spawn/keyframe — it still carries scale and initial
  placement); afterwards, motion is body state. This kills the double-send,
  and it makes the two windowed-host defects in §2 irrelevant rather than
  fixed-in-place: the render-side writeback can stamp whatever it likes,
  because replication no longer reads it. Non-bodied entities are untouched.
  The honest cost: a *non-pose* Transform mutation on a bodied entity after
  spawn — a runtime scale change is the realistic case — reaches clients only
  at the next keyframe sweep. Accepted for v1, because a runtime scale change
  on a live body needs a collider reshape the engine also does not do yet
  (§5); if it ever matters before then, the carve-out is to suppress the
  Transform block only on snapshots whose body-state section covers the
  entity.
- **The capture works headlessly**, because it reads the physics world, not
  the render writeback — closing the stale-pose headless gap for free.

**Apply (client, on receipt, before the tick's physics step).** For each
received body state: if the mirror has no body yet, build it
(`AddBodyFromDescriptor` from the replicated descriptor + Transform, at spawn
handling below); then `ApplyBodyState` — pose, velocities, and activation in
one call, `DeactivateBody` when asleep. The *simulation* is snapped hard;
Gaffer's reasoning applies verbatim — extrapolation must proceed from a valid
physics state, and the smoothing belongs to the view, not the sim
([State Synchronization](https://gafferongames.com/post/state_synchronization/)).
Before snapping, the client measures `|local pose − authoritative pose|` and
folds it into the visual error offset (§3.5) and the divergence telemetry
(R6). Corrections arrive inside the snapshot, so ordering with spawns in the
same packet is free: blocks first (entity + descriptor exist), body section
after (the body starts at the authoritative state, not a re-settle).

**Sleep, precisely (trap 1).** Three mechanisms compose, and each covers a
failure the others cannot:

1. **Sleep state is replicated state.** The `asleep` bit plus rest pose is
   sent on the sleep transition and — like any state — *resent until acked*
   by the baseline machinery. Loss cannot eat it.
2. **The client enforces the server's sleep verdict.** After its physics
   step, the client re-asserts: any mirror whose last authoritative state
   said `asleep` but whose local body is active gets snapped back to the rest
   pose and deactivated. This closes the local wake-cascade: client-side
   poses differ slightly under smoothing, so a settling pile can produce
   contacts the server never had, and Jolt wakes bodies by island — one
   spurious local contact would otherwise wake a mirror the server will never
   speak of again. Jolt's own behavior makes the enforcement cheap and stable:
   sleeping bodies stay asleep until contacted or explicitly activated, and
   sleep is governed by `PhysicsSettings::mTimeBeforeSleep` /
   `mPointVelocitySleepThreshold`
   ([Jolt, sleeping](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)).
   The legitimate wake path is the server's: a correction with `asleep =
   false` activates the body, and until it arrives the mirror holds still —
   a one-transit-time hesitation, accepted.
3. **The keyframe sweep** (§3.4) backstops what neither of the above can see:
   states that were delivered, acked, and *then* corrupted client-side by
   anything this plan failed to imagine — including client gameplay writing
   replicated fields (§3.5).

The nearest shipped analogue is Unreal's dormancy, which solves the same
"stopped-sending things drift" problem in the opposite direction — by
discipline: dormant actors are skipped entirely, and "an actor's replicated
state should not change while it is dormant, as these changes might be lost
when the actor is awoken," so correctness depends on every call site flushing
before mutation
([Actor Network Dormancy](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-network-dormancy-in-unreal-engine)).
v4's version is structural instead: "dormant" *is* "asleep", the transition
into it is itself replicated and acked, and the client enforces it. No call
site can forget.

### 3.4 Baselines, the keyframe sweep, and send priority

**Per-entity baselines (and the bug fix).** The per-connection baseline
stops being one global change tick and becomes per-entity:

```cpp
struct EntityBaseline
{
    std::uint64_t componentTick = 0;  // components delivered up to here
    std::uint64_t bodyTick      = 0;  // body state delivered up to here
};
// per Connection:
std::unordered_map<NetId, EntityBaseline> baselines;
```

Each in-flight `SentSnapshot` records, per entity it *actually wrote*, the
capture ticks it wrote at; an ack folds exactly those into the connection's
baselines. An entity skipped for budget simply keeps its old baseline — which
is the whole fix for the latent staleness bug in §2: "included in the record"
and "delivered at tick X" stop being conflated. The acked entity and
component *sets* — and the despawn/removal diffs and `worldComplete` built on
them — are untouched; only the tick bookkeeping changes shape. Two lifecycle
details that are part of the design, not implementation trivia:

- **Retired NetIds leave the map.** NetIds are never reused
  (`Replication.cpp:296-299`), so without cleanup, `baselines` grows with
  every entity that has *ever* replicated — unbounded under projectile-style
  churn. A NetId's baseline entry is erased when its despawn is acked (the
  entity leaves the connection's acked set while no longer live); never-reuse
  means a straggler ack cannot resurrect one.
- Memory is otherwise one map entry per live replicated entity per connection
  — two `std::uint64_t`s each — noise at the target scale.

**The keyframe sweep — demoted, kept, and honest about why.** The suggested
three-tier design made an unconditional keyframe the load-bearing answer to
trap 1. The code says otherwise: the acked-baseline machinery *already*
guarantees that every state change — including the final rest pose — is
resent until the client confirms it, the acked-set diff already self-heals a
lost despawn the same way, and §3.3's sleep enforcement covers the
post-delivery drift that delivery guarantees cannot. What remains for a
keyframe is the failure class nobody designs for: delivered-then-corrupted
state, a divergence path this plan did not foresee, the bug that made v2's
live test humiliating. Against that class, a periodic full re-anchor is cheap
insurance with a near-one-line implementation: every `keyframeIntervalTicks`
(default 512 — ~8.5 s at 60 Hz), reset every entity's baseline ticks for each
connection to zero **and clear that connection's in-flight ring**. The ring
clear is not optional tidiness: an ack for a pre-sweep snapshot arriving
*after* the sweep would fold that record's per-entity ticks back into the
baselines and silently cancel the re-anchor for exactly the entities it
covered. With the ring cleared, a late ack finds no record and is ignored
(the existing `HandleAck` shape already does this), at the cost of one
over-full resend — the correct direction to be wrong in. The existing
machinery does the rest — baseline 0 *is* the full-state path, and the byte
budget automatically paginates the sweep over however many snapshots it
needs. So the open question in the brief resolves concretely: **the keyframe
is not a third mechanism; it is the second mechanism with its filter reset**,
and it is a robustness knob rather than a pillar — config, disableable, but
the off position earns a warning in the config comment itself: disabling the
sweep saves ~0.6 kB/s (64 entities × ~80 bytes spread over 8.5 s — negligible
against the correction stream it backstops) and, for any client-side system
that writes replicated fields on mirrors (§3.5), converts "wrong until the
next sweep" into "wrong forever." A knob that saves almost nothing and
removes a safety net must say so where it is flipped.

**Send priority (trap 3).** Divergence is not uniform — contact-rich piles
amplify error chaotically while ballistic bodies drift gently — but the
design does not need a per-object *interval* to respect that, and building
one would be machinery for a problem the accumulator already solves. The
send loop keeps its existing "collect, order, drain to budget" shape and
finally implements the ordering: per connection, per dirty entity, an
accumulator gains `max(Replicated::priority, ε)` each snapshot tick (the
field has existed, unused, since N5; ε = 1/64). The clamp is not decoration:
the field's authored range bottoms out at 0.0 (`NetComponents.hpp:36`), and
a raw gain of zero means a zero-priority entity *never* climbs — silently
starved forever under budget pressure, which would falsify the guarantee one
sentence from now. With the clamp, 0 means "last in line," never "never."
Entities drain highest-first into the byte budget; **only the drained reset
their accumulators** — the not-sent keep climbing and cannot starve. This is the Tribes-lineage priority accumulator
exactly as the state-sync literature specifies it
([State Synchronization](https://gafferongames.com/post/state_synchronization/);
[Tribes Networking Model](https://www.gamedevs.org/uploads/tribes-networking-model.pdf)).
Under no budget pressure everything dirty goes every snapshot tick and the
accumulator is inert; under pressure, correction *frequency* degrades
smoothly, per object, steered by an authored per-entity number — the debris
pile at priority 0.5 yields to the door at priority 10 precisely when
bandwidth forces a choice, which is the honest version of "correction rate
tied to gameplay significance." Sleep transitions ride at a large fixed
priority bump: the final rest pose is the one update whose delay is
permanently visible.

### 3.5 The client's two kinds of entity, and error smoothing

**Bodied mirrors** (have a replicated `RigidBodyDescriptor`): real dynamic
bodies in the client's world, stepped by the normal fixed-tick physics,
rendered by the normal physics writeback, corrected as §3.3. They leave the
snapshot-interpolation path entirely — no transform history is recorded for
them.

**Non-bodied mirrors** (everything else — gameplay-moved transforms, and any
future replicated state without local simulation): exactly today's path,
untouched — three-sample history, two-interval delay, hold-don't-extrapolate.
The rule from §0 read backwards: where only one side runs the logic, the wire
carries the state, and state without local simulation is rendered by
interpolation, for which ~100 ms of buffer is the industry-settled answer
(Source's `cl_updaterate 20` / `cl_interp 0.1`
— [Source Multiplayer Networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking);
the page currently 403s anonymous fetches — numbers re-verified in the
in-repo research pass, `docs/research/networking/r5-review-external.md` §V10
— and Godot's synchronizer is the same property-streaming shape
([MultiplayerSynchronizer](https://docs.godotengine.org/en/stable/classes/class_multiplayersynchronizer.html))).
The two timelines coexist; the seam between them — a bodied entity resting on
an interpolated platform — cannot occur in v1 because nothing is both: the
engine has no kinematic bodies yet, and that deferral is listed in §5. The
milder visual form *can* occur — a non-bodied entity the server moves to
track a bodied one renders ~100 ms behind its target — and is accepted as
inherent to running two timelines. Nothing on screen says which timeline an
entity is on — the discriminator (a replicated `RigidBodyDescriptor`) is
invisible in the world — so R7 puts a replication-path line in the
inspector; "why do these two lag differently" should cost one click, not a
debugging session.

**Error smoothing (trap 4).** Per bodied mirror, the client keeps a visual
offset `{glm::vec3 positionError; glm::quat rotationError;}`. When a
correction snaps the sim, the offset absorbs the difference so the rendered
pose is unchanged at that instant (which is also why `ApplyBodyState` must
collapse the render-interpolation snapshots — §3.3); every rendered frame
thereafter the offset decays toward zero and is *added on top of* the physics
writeback's pose. Constants start at the published values and become config
the moment R6's telemetry gives a reason to move them: decay ×0.95 per frame
for position error ≤ 25 cm, ×0.85 for ≥ 1 m, blended between; orientation
error slerped toward identity ~0.1 per frame; a hard snap (no smoothing)
beyond 2.5 m, because smoothing a teleport reads worse than admitting it
(factors and thresholds from
[State Synchronization](https://gafferongames.com/post/state_synchronization/);
the 2.5 m snap bound is this plan's choice, not a citation). Placement: the
offset application runs *after* the physics writeback and before transform
propagation — in the editor loop that is immediately after
`InterpolateTransforms` in `OnRender`, and the offsets live in
`ReplicationClient` keyed by NetId. The existing `NetSession::Interpolate()`
entry point is replaced by `NetSession::SmoothView()` doing both jobs —
interpolation for non-bodied mirrors, offset decay for bodied ones — and its
call site *moves*: today it runs in `OnUpdate` (`EditorApp.cpp:833`), before
the writeback that would overwrite it; it must run after
(`EditorApp.cpp:633`'s block). A headless client skips it, and its
convergence assertions read body state, not smoothed Transforms.

**Client-side gameplay during a session.** The client is *in Play*: its own
gameplay systems run over the play world, mirrors included. That is the
decision taken seriously rather than a hazard to fence off — a client-side
`Bounce` firing on a mirror's contact is a local guess at what the server's
`Bounce` also did. But be precise about what reconciles which write, because
corrections are deltas, not streams: **body state is re-anchored continuously
while the server's body is awake; a replicated *field* the server is not
re-stamping is never resent by the delta path at all.** A client system that
writes such a field on a mirror is wrong until the keyframe sweep re-anchors
it — the same mechanism §3.6's editor guard exists for, now stated once for
both. So the rule, sloganized for the gameplay author who will never read this
plan: **clients may push mirrors, never set them.** In full:

| a client writes… | who re-anchors it | how fast |
|---|---|---|
| mirror body state (impulses, velocities, pose) | the correction stream | next correction — ≤ 1 snapshot interval while the server's body is awake |
| a replicated field the server keeps re-stamping | the delta path | the next snapshot that includes the entity |
| a replicated field the server is *not* re-stamping | the keyframe sweep only | up to `keyframeIntervalTicks` — ~8.5 s at the default |

Client gameplay may freely touch mirror *bodies* (the correction stream owns
them) and any unreplicated or cosmetic state; client systems that write
replicated non-body fields on mirrors are leaning on the sweep as their only
re-anchor, and a game that does so must leave the sweep enabled (§3.4). v1's
engine-side systems make the lean
theoretical — `Bounce` rewrites body velocity, nothing else — but the rule is
recorded so it cannot become load-bearing by accident. One more consequence
of gameplay-touches-mirrors, specified rather than discovered: a client
system may `Destroy` a mirror. The apply path must tolerate it — today
`ApplySnapshot` dereferences the mapped handle with no `IsAlive` check
(`Replication.cpp:679-697`); v4's client drops a dead mapping and re-runs the
existing unknown-NetId-as-spawn path, so a locally-destroyed mirror
resurrects at the next update or sweep instead of corrupting the apply. And
resurrection is *counted, not silent*: an R6 panel counter ("mirrors
resurrected") plus one log line on first occurrence — because from the
gameplay chair "Destroy didn't destroy" is spooky, and a client-side cleanup
system (a kill-Z volume, a timed despawner) running over mirrors would
otherwise produce a quiet destroy/respawn churn loop with no signal anywhere.

The same taken-seriously rule gives unmarked level dynamics their v4 meaning:
the client builds bodies for them and simulates them as **cosmetic local
physics** — they may settle differently per machine, nobody corrects them,
and their *contacts with replicated bodies* exist only locally: a cosmetic
crate that rolls against a sleeping mirror fights §3.3's sleep enforcement
and can jitter or come to rest at a per-client pose the server never saw.
The R7 host warning tells the author which bodies they forgot to mark, and
its wording names the contact artifact, not just the drift.
Deriving cosmetic physics locally instead of networking it is standard
practice — the Halo networking talk's summary lists "change gameplay to
require less networking (e.g. don't network ragdolls)" among its rules
([GDC 2011 summary](https://www.wolfire.com/blog/2011/03/GDC-Session-Summary-Halo-networking)).

### 3.6 Editor integration: joining is a Play mode

One rule replaces v3.5's §2: **a network session exists only inside a play
session.** Hosting starts by entering Play; a client joins by entering Play
with a join target; `StopPlay` — either side, any reason — tears the session
down. The join builds its world inside the *play* scene, which the editor
already treats as disposable, so the machinery v3.5 §2 hand-built mostly
already exists:

| v3.5 §2 mechanism | v4 disposition |
|---|---|
| Save disabled while joined | already exists — Save/Save As/Open are gated on `PlayState::Editing` (`EditorLevels.cpp:66,87,97`) |
| "Join discards unsaved changes" confirm | gone — the play snapshot preserves the editing scene; nothing is discarded |
| Disconnect reloads level from disk | gone — `StopPlay`'s `ReviveAt` restore returns the *exact* pre-play scene, unsaved edits and undo history intact (`EditorPlay.cpp:103-171`), strictly better than a disk reload |
| Client-may-not-press-Play guard | moot — the client is in Play; the guards that remain are Pause and world structure (below) |
| Dirty-client-scene handling | gone — the join never touches the editing scene |
| Level identity + hash in `ServerHello` | **kept** — genuinely new state the handshake needs (below) |
| Unconditional load-from-disk + strip | **kept** — now trivially safe: it builds the disposable play scene |
| Strip orphan semantics (`Parent` cleanup) | **kept** — one paragraph, unchanged from v3.5 |
| Deferred `ClientHello` until level ready | **kept** — the ordering hazard is real (stale NetId maps aliasing live entities) |
| Structure-dirty asset resolve | **kept** — mirrors must draw (v2 gap 3) |

**The join sequence** (client, all inside Play): press Play with a join
target → normal play snapshot of the editing scene → connect → wait for
`ServerHello` (a visible "Joining…" sub-state; a timeout aborts) → verify
protocol hash → marshalled to the frame safe point: clear the play scene,
load the handshake's level from disk, **verify the content hash** (mismatch
aborts with the *actionable* message — "your copy of `<level>` differs from
the host's; sync the file from the host and retry" — while the two hash
values go to the log, since they only answer "are they different," which the
failed join already announced), strip entities carrying
`Replicated` (clearing orphaned children's `Parent`, which otherwise dangles
into local-as-world rendering), resolve assets, build physics — statics and
unmarked dynamics; marked ones arrive as mirrors — → send `ClientHello` →
snapshots and corrections flow (into a deliberately spectator v1 — §5). Any failure on that path is `StopPlay`, which
is also the answer to "how do I get out of a bad join": the same button as
always. `ServerHello` gains the level fields (explicit-width, tagged
addressing — never sniffed):

```cpp
struct ServerHello
{
    // ... existing fields ...
    std::uint8_t  levelAddressing  = 0;  // 0 none, 1 virtual path, 2 absolute temp path (PIE)
    std::string   levelPath;             // interpreted per levelAddressing
    std::uint64_t levelContentHash = 0;  // Core::ContentHash64 of the file as saved
};
```

**Host side:** Play-and-listen requires a saved level (its handshake is the
disk path; the demo scene or a never-saved level refuses with "save the level
to host"). Hosting with unsaved edits raises a **modal, not a warning**: "Save and
host" (the default button) / "Host last-saved" / "Cancel". v3.5's amber
warning was the wrong shape for this failure: clients load the last *saved*
file, so the host's unsaved wall is elsewhere on every client, replicated
bodies get corrected against server geometry the clients cannot see, and
"objects bouncing off nothing" minutes later never gets traced back to a
warning glanced past at host time — one dialog kills the class. The real fix
(shipping in-memory scenes) remains a level-transfer design nobody needs
before cross-machine use is real (§5); PIE avoids all of it structurally via
the temp snapshot. The headless host
already knows its level path and sends it; a headless host with no level
sends `levelAddressing = 0`, which a joining editor treats as a load failure
and aborts cleanly.

**Pause and world structure are disabled while a session is active** — both
roles, with tooltips. Pausing a host stops the server ticking under connected
clients; pausing a client stops correction application under a live stream;
neither is a state this plan wants to define, and un-pausing semantics for a
networked session are a deferred design, not a v1 casualty. The same rule
extends to the Game panel's world-structure operations — "Load as new world",
"Travel here", the seamless-load Prepare/Load-now pair, "Destroy this world",
and Migrate — all of which are live during Play today
(`EditorPlay.cpp:381,400,429`) and applied with no session interaction
(`EditorApp.cpp:760-763`), while the session binds its scene by reference at
construction (`Replication.cpp:68-69`). A host-side Travel mid-session would
either dangle that reference or keep replicating a retired world; a
client-side one detonates the join contract. v1 disables them in-session;
host travel as a proper mid-session level renegotiation is deferred (§5).

**Mirrors in the editor.** Mirrors get the transient `Mirrored` tag; one
shared `IsEditable(entity)` predicate (≈ not Mirrored) gates the inspector,
gizmo, Delete key, and hierarchy actions, and mirrors never enter edit
history. The scope of this guard shrank with the session-viewer's removal —
it protects a play-mode scene now, not the editing scene — but it stays,
because a gizmo fighting the correction stream is a confusing artifact even
in a disposable world, and because the server resends only what changes: a
client edit to a static replicated field would sit wrong until the keyframe
sweep — the same delta-path property §3.5 states as the client-write rule —
which is exactly the class of quiet wrongness this plan spends machinery to
avoid.

**PIE** carries over from v3.5 §4 nearly verbatim, trimmed of the parts the
session-viewer's removal obsoleted: the Play control's net dropdown
(Standalone / Host / Host + 1/2/3 clients / **Join…** with an endpoint
field, sticky per session, reset at launch — Join lives in the same dropdown
as Host deliberately, so both halves of one feature share one surface and
"where do I join from?" is answered "same place you host from"; the Network
panel remains the detail/stats view, not a second place sessions start), the
temp-level snapshot as the PIE handshake (absolute path,
`levelAddressing = 2`), `App::ChildProcess` (spawn/terminate/reap, POSIX
first), `--pie-client` as a restricted viewer (no shared-file writes:
read-only asset DB, per-process or absent `imgui.ini`, no `options.json`
writes; `PR_SET_PDEATHSIG` so an editor crash leaks no windows), SIGTERM →
grace → SIGKILL → reap → delete temp file on Stop. Perceptual judgments on
Host + 1 only; Host + 2/3 judged functionally — four swapchains on one GPU is
contention, not netcode. The accepted iteration cost is unchanged and worth
restating: every host Stop disconnects every client and PIE children respawn
as fresh processes per Play cycle; that is the price of "connections do not
outlive the level."

### 3.7 Numbers: rates, budgets, precision

**Rates.** Sim stays 60 Hz. Corrections ride the existing snapshot cadence,
default 20 Hz, still clamped to a divisor of the tick rate (the judder
argument in `Replication.cpp:71-88` stands). Under local simulation the
snapshot rate stops governing visual smoothness — the client's own 60 Hz sim
fills the gaps — and becomes purely the re-anchor cadence: at 20 Hz, drift
has 50 ms + transit to grow between anchors, which the R6 telemetry will
either bless or indict. Raising it is a config change, not a design change.

**Packet budget.** `maxSnapshotBytes` stays ~1100. GNS will happily carry
unreliable messages above one MTU, but "if any piece of the message is lost,
the entire message will be dropped"
([steamnetworkingtypes.h](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingtypes.h))
— multi-MTU snapshots multiply their own loss rate, so one-packet snapshots
remain the policy and the priority accumulator remains the mechanism that
makes the budget a scheduling decision instead of a truncation.

**Bandwidth, computed.** Whole-float R5: awake body ≈ 54 B, asleep ≈ 30 B.
Worst case 32 awake bodies × 54 B ≈ 1.7 kB — over one budget, so the
accumulator rotates the set across two snapshots (effective 10 Hz per body at
full churn, 20 Hz below ~20 awake bodies). Per-client ceiling at 20 Hz is a
flat 22 kB/s ≈ 176 kbps — comfortable for the 2-8 player near-term and fine
at 32 on a hosted box. Settled worlds cost headers: the existing "<24 bytes
per idle snapshot" bound becomes true *for physics* for the first time.
Quantized (R8) the awake record drops to ~22 B — 32 bodies ≈ 700 B, whole
world in one budget — using exactly the published encodings: smallest-three
quaternions at 2+9+9+9 = 29 bits, bounded positions at ~2 mm (Gaffer ships
18/18/14 bits over a 512-units-per-meter world; ours parameterize from a
config), an at-rest bit in place of velocities
([Snapshot Compression](https://gafferongames.com/post/snapshot_compression/)).
His caution that state-sync extrapolation wants *finer* quantization than
snapshot interpolation (4096/m, 15-bit components in the state-sync article)
is exactly the tuning question R8's measurement answers before bits are
chosen. One deliberate omission: his quantize-both-sides trick — feeding
quantized state back into both simulations so they extrapolate identically —
is skipped; it exists to make extrapolation *exactly* match across machines,
which without determinism ours never will, and the always-on smoothing
absorbs what it would have prevented. Round-to-nearest quantization does not
accumulate error across corrections — each re-anchor lands within half a
quantum, independently — so precision is a display-quality knob, not a
correctness one (`BitStream.cpp:135-138` already rounds to nearest for this
reason).

**Fast-math (trap 2), restated as a closed question.** The flag stays on;
the design needs bounded drift, not agreement; drift is bounded by the
correction stream at a measured, telemetry-visible rate; and §3.1's
determinism analysis shows that even turning fast-math *off* would buy
nothing this architecture could spend. `CMakeLists.txt:192-194` already
records the dependency in the right direction.

## 4. Milestones

Strictly serial, each ending in something a human watches work — R4's
"watcher" is the test suite, and it is the one deliberate exception, argued
in place. R-numbers, so nobody confuses them with v3.5's M-numbers or v2's
N-stages.

> **R1's opt-in model is superseded by docs/replication-optin-plan-v1.md**
> (built, 2026-08-02). What changed: `ACOMP(replicated)` is now
> `ACOMP(replicable)` and grants a *capability* rather than declaring a policy,
> with policy moved to the game's `neverReplicate` list and each entity's
> `Replicated::excluded` mask. The defect R1 shipped with was fusing the two —
> marking `Physics::Bounce` to serve one test level made it wire traffic for
> every game on this engine, from inside a physics module that cannot know any
> game's policy. Everything else in R1 (the flags in the metas, the hash folding
> them in, the server filtering on them, the `Mirrored` tag) stands unchanged,
> and the rest of this document remains the plan of record for R2–R9.

**R1 — Wire gating.** reflectgen learns `ACOMP(replicated)` (implies
`tracked`; `replicated`+`transient` is a generation error) and
`AFIELD(norep)` (error outside a replicated type; error with `transient`);
`ComponentMeta`/`FieldMeta` gain the flags; `ProtocolHash`/`ProtocolSummary`
fold them in; `ReplicationServer` filters on them; the transient
`NetSync::Mirrored` tag lands. Marked initially: `ECS::Transform`
(`replicated, tracked` — already tracked), `Runtime::MeshRenderer`,
`Runtime::Name`, and — reversing v3.5 — `Physics::RigidBodyDescriptor`,
because R5's client builds bodies from it. The NetSync test-support
components (`tests/support/.../TestNetComponents.hpp`) migrate to
`ACOMP(replicated, ...)` in the same commit — the opt-in flip unmarks them,
and without the migration the 45-case suite goes red, which R1's DoD treats
as in-scope, not collateral. Not `Camera` (the `isActive` hijack in §2 is
this milestone's poster child), not `Parent` (deferred for hierarchy
semantics; its EntityRef machinery stays tested and dormant), not `Bounce`
(client-local guess is fine — §3.5). *Why first:* R2 puts mirrors on screen;
without R1 every serializable component arrives with them, including the
ones that break the client watching.
*DoD:* tests prove an unmarked type never travels, a `norep` field holds its
client default while siblings update, disk round-trip of `norep` fields is
unchanged, differing annotations produce differing hashes; the full suite is
green (fixtures migrated); live: a sandbox host/client pair shows mirrors
carrying exactly the marked set — and a marked entity with a `Camera` no
longer moves the client's view. Untracked-type delta staleness (§2,
`MeshRenderer`) dies here via implied `tracked`; a test pins a post-spawn
`MeshRenderer` change arriving.

**R2 — The join that works, Play-bound.** Level identity + hash in
`ServerHello`; the join-as-play sequence of §3.6 end to end (snapshot →
connect → hello-gated load → strip with orphan cleanup → resolve + physics
build → `ClientHello`); host-requires-saved-level plus the unsaved-edits
host modal (§3.6); `StopPlay` teardown both
sides; client auto-`StopPlay` on disconnect/timeout; Pause *and the Game
panel's world-structure operations* disabled while a session is active
(§3.6); structure-dirty asset resolve so mirrors draw; the
`EditorApp.cpp:663` pump comment rewritten to the Play-bound rationale. Panel
keeps Join/Disconnect/stats; its Host and Join buttons become Play-gated
scaffolding until R3's dropdown absorbs both (§3.6 — one surface for both
halves of the feature), after which the panel is the detail/stats view only. Motion still flows through the old
Transform-interpolation path — bodies come in R5 — so mirrors move exactly as
today's tests move them.
*DoD:* editor A hosts `Materials.alvl` in Play with one marked moving entity;
editor B joins from both starting states (nothing open / same level open) —
level appears, entity moves smoothly, nothing duplicates; B's editing scene
comes back intact after disconnect (its unsaved pre-join edits included —
the thing v3.5 couldn't do); A's Stop ends B's session with the same
restore; hash mismatch and load failure both abort into a clean `StopPlay`
with a panel message naming the cause; while either side's session is
active, Pause, Travel, Load-as-new-world, seamless load, Destroy-this-world,
and Migrate are visibly disabled with tooltips.

**R3 — PIE.** The §3.6 dropdown, temp-level snapshot, `ChildProcess`,
`--pie-client` restrictions, teardown discipline. *Why here:* it converts
every R5-R8 verification from two-window choreography into one click, which
is the v3.5 ordering argument unchanged, and R5 is the milestone that most
needs cheap repeated eyeballs.
*DoD:* with unsaved host edits, Play → "Host + 1" opens a window that
auto-joins and shows the moving world including those edits; Stop closes it;
3× repeat with no zombies (`ps` verified), no port clash, no child writes to
`imgui.ini`/`options.json`/sidecars (mtimes verified); "Host + 3" connects
three, one closing leaves two, Stop reaps all — judged functionally.

**R4 — Per-entity baselines and the keyframe sweep.** The §3.4 bookkeeping,
against the *existing* replication model — no physics anywhere in this
milestone: `EntityBaseline` maps replace the global `ackedChangeTick`;
`SentSnapshot` records per-written-entity capture ticks and acks fold exactly
those; baseline entries are erased on acked despawn; the keyframe sweep
(`keyframeIntervalTicks`, default 512) as baseline-reset + in-flight-ring
clear, budget-paginated by the machinery it reuses. *Why its own milestone,
and why here:* the budget-staleness bug is a correctness defect in the
shipped core, independent of everything physical — splitting it out lands
the fix earliest and takes the riskiest bookkeeping change out of R5, which
is already the largest milestone. Its watcher is the test suite rather than
a window, accepted once and deliberately: there is nothing visual about a
baseline.
*DoD (all library-level):* a regression test in which an entity's *final*
change lands in a budget-starved snapshot converges — written first and
demonstrated to fail against the old code; the sweep re-anchors a
deliberately-corrupted mirror field within one interval; a late ack for a
pre-sweep snapshot does not cancel a sweep (the ring-clear test); a
despawned entity's baseline entry is gone after the despawn acks; the
150 ms/5 % soak and the full suite stay green.

**R5 — Client-side physics with authoritative correction.** The heart.
NetSync links Physics; `PhysicsWorld` gains the §3.3 passthroughs
(`GetActiveBodyStates`, `IsBodyActive`, `DeactivateBody`, `ApplyBodyState`
with the interpolation-snapshot collapse); server-side body-state capture
(active set + sleep transitions + dirty-init on NetId assignment); the
body-state snapshot section with the known-entity inclusion gate;
Transform-block suppression for bodied entities; client body construction
from descriptors, hard-snap apply, unknown-NetId skip, dead-mapping
tolerance (locally-destroyed mirrors respawn — §3.5), sleep application and
enforcement; a minimal host-side debug impulse (a panel button applying an
impulse to the selected replicated body), which this and later DoDs lean on.
Corrections apply unsmoothed this milestone — R6 owns feel; small pops under
lag are expected and visible, which is honest.
*DoD (library):* two Scenes + two PhysicsWorlds over loopback: a spawned
pile settles on the server → client bodies converge within ε *and go to
sleep*, and idle snapshots return to the <24-byte bound **with physics
running** — the sentence §2 shows is false today becomes a test; a
force-woken client body is re-slept at the authoritative pose within one
tick; a locally-destroyed mirror is restored by the next update; the
150 ms/5 % soak converges.
*DoD (visible):* PIE Host + 1 on a level with a marked pile: both windows
settle to the same arrangement; the host's debug impulse pushes a body and
the client follows within a correction interval; a headless host drives the
same scene correctly (the §2 stale-pose gap dead).

**R6 — Feel and telemetry.** Error smoothing per §3.5 (offsets, decay
constants, snap threshold; `NetSession::Interpolate()` replaced by
`SmoothView()` with its call site moved after the physics writeback); the
priority accumulator over `Replicated::priority` with sleep-transition
boost; panel telemetry: correction bytes/sec, corrections applied/sec, mean
and max divergence-at-correction, per-connection dirty backlog, and the
mirrors-resurrected counter with its first-occurrence log line (§3.5); a
`MessageType::RequestKeyframe` control message (client → server; zeroes that
connection's baselines and clears its ring, same path as the sweep) behind a
"force full resync" panel button; a debug "corrupt mirror" poke (client-side
pose scramble on the selected mirror) so the resync affordance has something
honest to heal. *Why after R5:* smoothing tuned before correctness is pinned
tunes against bugs, and the divergence numbers this milestone surfaces are
the measured basis trap 2 demands for the correction interval.
*DoD:* under 150 ms/5 % simulated conditions in PIE Host + 1, a disturbed
pile shows no visible pops or teleports at correction arrival (judged by
eye, Host + 1 only); the panel's divergence figures move plausibly with
simulated latency; force-resync visibly heals a corrupted-by-the-poke client
within one sweep.

**R7 — Authoring UX.** The inspector "Replicated" checkbox (undoable, through
the normal edit path); wire glyphs on `ACOMP(replicated)` component headers
plus the "not replicated — type lacks ACOMP(replicated)" note; zero-
replicated-components inline warning; parented-entity and has-children
warnings (strip semantics); the panel's bound-endpoint display, negotiated
level display, zero-marked-entities warning, and the **unmarked dynamic
bodies** warning ("these simulate locally as cosmetic physics — they will
settle differently per client and their collisions with replicated bodies
happen only on this machine; mark them Replicated to synchronize"); NetId
shown in the inspector on both ends; a replication-path line on mirrors
("body-corrected" / "interpolated") so §3.5's two timelines are diagnosable
from the inspector instead of inferred from a descriptor's presence; dimmed
`norep` fields; `Mirrored` hierarchy tint +
read-only via the single predicate + history exclusion.
*DoD:* an entity replicates with one click and no code; every warning above
is reproducible on demand; a mirror cannot be edited by inspector, gizmo,
Delete, or hierarchy actions and never appears in undo history.

**R8 — Quantized corrections.** The body-state field encoders switch to
`WriteFloatQuantized`/smallest-three per §3.7; world position bounds and
velocity ranges land in config and inside `ProtocolHash`; the R6 panel
numbers before/after are the milestone's evidence. *Why last of the feature
milestones:* the encoders are drop-in (the primitive has been ready since
N4), the ranges deserve to be chosen against R6's measured divergence and
bandwidth rather than guessed, and same-binary + hash-refusal makes late
format changes free — the usual reason to quantize early (shipped-format
inertia) does not exist here.
*DoD:* correction bytes/sec drops ≥ 2.5× on the R5 pile scene; the 150 ms/5 %
soak and the by-eye R6 pass hold at the reduced precision; a build with
different bounds refuses to pair, with the summary naming the field.

**R9 — Verification and docs.** The full matrix self-driven: windowed host +
PIE client + headless client simultaneously; disconnect/rejoin; Stop-side
teardown from each role; hash-mismatch and load-failure aborts; keyframe
sweep on/off; the soak. Then update `networking-design-notes.md`'s status
block, `remaining-work.md` §1, and the project memory; v3.5 gets a one-line
header pointing here.
*DoD:* the matrix passes end to end in one sitting, and the docs say what is
actually true.

## 4a. What is verified, and what still needs eyes

Written 2026-08-01, after R1-R8 landed. The split is not "tested vs untested" —
it is **what a terminal can decide vs what a person has to look at**, and
keeping those apart is the only way "done" stays a claim rather than a hope.

**Verified, and how.** Every library-level DoD in R1, R4, R5, R6 and R8 has a
test, and the two that pin *fixes* were confirmed to fail against the old code
before the fix went in (the budget-staleness regression, and the sweep's
in-flight ring clear). Over real UDP, headlessly: a host/client pair negotiates
`levels/NetPile.alvl`, verifies its content hash, strips the six authored
copies, and mirrors them with zero rejects; a one-byte edit to the client's copy
aborts the join with both hashes logged; a headless client and a windowed
`--pie-client` join the same host simultaneously; a client disconnects and
rejoins a live host; and the correction stream falls from ~52 to ~35 bytes per
snapshot with quantization on, settling to headers once the pile sleeps.
`App::ChildProcess` is tested against real processes, including the case where
a child exits on its own and the case where the executable does not exist.

**Not verified, and it needs a person.** All of it is either a by-eye judgement
or a two-window sequence:

- **R2** — editor A hosts in Play, editor B joins from both starting states
  (nothing open / same level open); B's editing scene and unsaved edits come
  back on Stop; A's Stop ends B's session; the disabled Pause / Travel /
  Load-as-new-world / seamless-load / Destroy-this-world / Migrate controls and
  their tooltips; the unsaved-edits host modal and its three buttons.
- **R3** — the Play control's net dropdown; "Host + 1" opening a window that
  auto-joins and shows unsaved edits; Stop closing it; three repeats with no
  zombies (`ps`) and no port clash; "Host + 3"; the client's camera framing.
  (The *absolute-path* temp-snapshot branch of the join is reachable only this
  way — the headless client speaks virtual paths only, so that branch has been
  compiled but never executed.)
- **R5** — PIE Host + 1 on `levels/NetPile.alvl`: both windows settling to the
  same arrangement, and the panel's "Nudge selected body" pushing a crate that
  the client follows within a correction interval.
- **R6** — the judgement the whole milestone exists for: under 150 ms / 5 %
  simulated conditions, a disturbed pile shows no visible pops at correction
  arrival. Also the panel's divergence figures moving plausibly with latency,
  and "corrupt selected mirror" → "force full resync" healing on screen.
- **R7** — every item; it is all UI.

## 5. Explicitly deferred

- **The authored level as a shared baseline — replacing strip-and-resend.**
  Today a joining client loads the level, then strips every entity carrying
  replicated components and receives them back from the host. The reason is
  narrow and stated at `EditorNet.cpp:176-200`: the level file's copies are
  the host's authored originals, "so keeping both would double every
  replicated object in the world." It is de-duplication, not a safety
  mechanism — the stale-NetId hazard is a *different* problem, solved
  separately by `SetDeferHandshake` (`Replication.hpp:1143-1155`).

  The better model: **both machines already hold the same hash-verified level
  file, so it is a shared baseline and everything on the wire is a delta
  against it.** NetIds for authored content are assigned deterministically at
  load — entity *i*, or instance *i* member *j* — identically on both sides,
  so nothing has to be told which entity is which. The host then sends only
  what changed since the level loaded, filtered by relevancy as usual.

  Most of it already exists. A delta baseline is just a change tick, so "what
  changed since load" is the query that already drives every snapshot, seeded
  from the load tick instead of from empty; relevancy filtering is unchanged.
  What has to be built: deterministic NetId assignment at load on both sides,
  an explicit **destroyed list** (a deleted entity has no components to delta,
  so deletions cannot fall out of the tick query), and seeding the join from
  the load tick.

  Deferred because it is a rework of the join path affecting every level,
  while the current path works. What it deletes when it lands: the strip
  itself, and `BlueprintSpawnFromLevel` — the provisional record that exists
  only so a stripped level instance can be rebuilt with its overrides
  (`docs/blueprint-system-concept.md` §9). Blueprints exposed this; they did
  not cause it.

  Related and further out: **world streaming** — a client loading only what is
  near it rather than the whole level. That is chunking, load/unload as the
  player moves, and a per-client notion of what is resident; it motivates the
  baseline model but is a much larger feature, and the baseline model does not
  depend on it. v1 assumes a client loads the whole level.
- **Player pawn possession, prediction, and resimulation** — the Rocket
  League/UE-Resimulation architecture (§3.1 option 3), with its measured
  cost. Waits on the Game/GameEditor template split (Phase 2), which owns
  "what is a player" — and v4's correction stream is the substrate it layers
  onto, not a thing it replaces. Until it lands, the expectation to set out
  loud: **a v1 joiner is a spectator.** It watches an authoritative world it
  cannot yet push — the input-command pipe is built, hardened, and consumed
  by nothing. If a client-side verb is wanted before the pawn, the cheap
  first step is exposing R5's debug impulse client-side as a *request*
  through that pipe, which would also be the input channel's first
  end-to-end exercise.
- **Kinematic bodies / moving platforms** — the engine has Static and
  Dynamic only; a networked platform needs kinematic support first, and the
  bodied-on-interpolated timeline seam (§3.5) becomes real then, not before.
- **Mid-session level change (host travel)** — v1 disables the editor's
  world-structure operations while a session is active (§3.6); a host
  changing level under connected clients is a renegotiation of the
  `ServerHello` level contract (teardown-and-rejoin semantics at minimum)
  and is designed when a game needs it, not speculatively.
- **Runtime collider reshape / scale replication for bodied entities** —
  post-spawn Transform mutations on bodied entities reach clients only via
  the keyframe sweep (§3.3); the realistic case (scale) also needs a
  collider reshape path the engine does not have. One design, later, for
  both.
- **Parent / hierarchy replication** — unchanged from v3.5, same honest
  reason: the EntityRef wire machinery works and stays tested; what is
  unsolved is semantics (mirrored children of local parents, strip
  interaction, transform spaces — and now world-space body state under
  parent-relative Transforms). Mirrors are flat in v1.
- **RPCs** — **no longer deferred.** Built as messages in
  docs/replication-messaging-relevancy-plan-v1.md (M3–M5): reflected structs
  under `AMSG(direction, reliability)`, one validated dispatch site for
  client→server intents, a snapshot section for server→client events. The
  deferral's own reasoning is what the design was built around — validation is
  security surface, not a rider — and the state-first rule stands unchanged:
  nothing that has a current value becomes an event.
- **The Steam provider, and the `NetProvider` seam with it** — v3.5 §5 built
  the seam early "while the session API is being shaped"; v4 cuts it, which
  is a change worth justifying: the join surface is two call sites
  (`NetSession::Join`, the panel), the Steamworks build exposes the same
  `ISteamNetworkingSockets` API by design — the relink is the transport swap
  ([Steam networking docs](https://partner.steamgames.com/doc/features/multiplayer/networking))
  — and a seam maintained unused through the entire v4 arc is speculative
  generality of exactly the kind v3.5 accumulated. When Steam lands, the
  endpoint-string abstraction gets cut against a real second implementation.
- **Adaptive time dilation** — the NetClock's inputs already exist; the
  smooth response (Overwatch's ~15.2 ms frames under starvation, Rocket
  League's upstream throttle) remains a drop-in later.
- **Interest management** — **no longer deferred.** Built as relevancy in
  docs/replication-messaging-relevancy-plan-v1.md (M1–M2): one sorted set per
  connection, intersected with the live set before priority, with a Distance
  provider and the four escape classes. The seam this plan reserved — a
  per-connection predicate applied before priority accumulation — is where it
  landed.
- **Lag compensation, reconnect-as-repair, sub-tick evaluation** — unchanged
  from the v2 deferral list, unstarted by design.
- **Windows** — the GNS chain has never built there;
  `protobuf_MSVC_STATIC_RUNTIME` defaults ON and will fight the dynamic CRT
  (tracked in `remaining-work.md` §1). Deferred, not forgotten.
- **Cross-machine host-with-unsaved-edits beyond the save prompt** — needs
  level transfer; the host-time modal (§3.6) makes the state explicit, and
  PIE covers the solo case structurally.

## 6. Open questions — the owner's calls

Assumptions are marked; the plan proceeds on them unless overruled.

1. **Correction cadence default.** Assumed: keep `snapshotHz = 20`. With
   local sim it governs re-anchor latency, not smoothness; R6's divergence
   telemetry is the evidence for moving it. 30 Hz costs +50 % correction
   bandwidth for 17 ms tighter anchoring.
2. **Where world bounds live for R8.** Assumed: a `networking` block in
   `game.json` (position bounds, max linear/angular velocity), folded into
   `ProtocolHash`. The alternative — per-level bounds in the level header —
   is more precise and more machinery; deferred until a level actually needs
   it.
3. **Pause during a session.** Assumed: disabled both roles (§3.6). The
   alternative worth wanting someday — host pause pauses everyone — is a
   protocol feature (a paused flag in snapshots) and is cut from v1.
4. **Unmarked dynamic bodies.** Assumed: cosmetic local physics + a warning
   (§3.5). The stricter alternative (freeze unmarked dynamics at file pose on
   clients) reads as *more* broken in-world and was not taken.
5. **PIE client camera auto-framing** (v3.5 M3 detail). Assumed: kept — a
   viewer window that opens staring at nothing undermines the one-click
   demo it exists for. Cheap, but it is UI opinion, so it is listed.

## 7. Where this plan disagrees with the brief's suggestions

The suggested approach was tested against the code and the literature;
here is what survived and what did not.

1. **The three-tier structure: partially rejected.** Tiers 1 and 2 stand as
   proposed. Tier 3 — the unconditional keyframe as the load-bearing answer
   to sleeping-body divergence — does not survive contact with the existing
   machinery. The trap's stated mechanism ("the server sees it as unchanged
   and stops sending, so it stays wrong permanently") assumes change is
   measured against the previous tick; this codebase measures it against the
   *per-connection acked baseline* (`Replication.cpp:374-377`, `215-231`),
   so the final rest pose is resent until the client provably has it —
   delivery cannot be the failure. The same goes for the brief's other tier-3
   rationale, the live entity set: a dropped despawn already self-heals
   through the acked-set diff (`Replication.cpp:406-411`), no keyframe
   needed. What delivery cannot fix is what the client does *after* delivery
   (local drift, spurious wakes), and those are closed structurally by
   replicated sleep state plus client-side sleep enforcement (§3.3). The
   keyframe survives demoted: a periodic baseline-zero sweep, near-one-line
   of mechanism, default-on as insurance against the unforeseen — and the
   brief's own suspicion that "tier 3 is simply tier 2 with the change-tick
   filter disabled" is confirmed exactly; there is no third mechanism at
   all. Meanwhile the *actual* trap-1-shaped defects found while verifying
   were elsewhere: sleeping bodies never stop replicating on a windowed
   host, and the budget path can mark undelivered changes as delivered (§2)
   — both now fixed by design (R4 and R5) rather than papered over by a
   heartbeat.
2. **Per-object correction interval: rejected as a property, kept as an
   outcome.** A configured interval per object is machinery the priority
   accumulator makes redundant: when bandwidth is not binding there is no
   reason to correct less often than every snapshot tick, and when it binds,
   fairness requires the accumulator anyway — at which point per-object
   *priority* (the field that has sat unused on `Replicated` since N5)
   produces per-object correction rates emergently, degrading the debris
   pile before the door. This is the mechanism the state-sync literature
   ships for exactly this problem
   ([State Synchronization](https://gafferongames.com/post/state_synchronization/)).
3. **"Quantized delta corrections" as tier 2's definition: re-sequenced.**
   Corrections ship whole-float in R5 and quantize in R8, after telemetry
   exists. The brief's own analysis (round-to-nearest cannot accumulate) is
   confirmed by the implementation (`BitStream.cpp:135-138`) — which is
   precisely why quantization is a bandwidth knob, not a correctness
   foundation, and bandwidth decisions made before the first measurement
   are guesses. Same-binary targeting plus hash-refusal makes the late
   format change free, so nothing is being deferred *onto* a cost.
4. **"Local simulation renders the present": tightened.** The mirrored world
   renders at server-time-minus-transit (§3.1) — the two-interval structural
   delay dies, which is the decision's substance, but claiming "the present"
   would be overselling: present-time shared physics requires
   rollback-resimulation, whose price the Rocket League slides state
   plainly, and which is deferred with the pawn. The plan records the
   honest ledger: what v4 removes (100 ms of structural buffer at defaults,
   plus inter-snapshot stepping), what remains (transit, smoothing).
5. **The v3.5 inheritance the brief did not ask about: mostly discarded.**
   The session-viewer mode and its ten sub-rules collapse into the play
   snapshot/restore (§3.6's table, verified against the play and save code)
   — the collapse trap 8 hoped for is real, and it deletes more of v3.5 §2
   than it keeps. The `NetProvider` seam is cut with its reasoning stated
   (§5). And one v3.5 decision is outright reversed with the model:
   `RigidBodyDescriptor` replicates, because the client now builds what the
   server describes.

## Sources

- Gaffer On Games — State Synchronization: <https://gafferongames.com/post/state_synchronization/> · Snapshot Compression: <https://gafferongames.com/post/snapshot_compression/> · Deterministic Lockstep: <https://gafferongames.com/post/deterministic_lockstep/> · Introduction to Networked Physics: <https://gafferongames.com/post/introduction_to_networked_physics/> · Fix Your Timestep: <https://gafferongames.com/post/fix_your_timestep/>
- Jolt Physics — Deterministic Simulation & sleeping (Architecture docs): <https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md> (rendered at <https://jrouwe.github.io/JoltPhysics/>) · active-bodies accessor caveats: <https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/PhysicsSystem.h>
- Unreal Engine — Networked Physics Overview (replication modes): <https://dev.epicgames.com/documentation/en-us/unreal-engine/networked-physics-overview> · Actor Network Dormancy: <https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-network-dormancy-in-unreal-engine>
- Rocket League — Cone, *It IS Rocket Science!* (GDC 2018 slides): <https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf>
- Overwatch — Ford, *Gameplay Architecture and Netcode* (GDC 2017): <https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and> · numbers via <https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback>
- Unity Netcode for Entities — Introduction to prediction: <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/intro-to-prediction.html>
- Godot — MultiplayerSynchronizer class reference: <https://docs.godotengine.org/en/stable/classes/class_multiplayersynchronizer.html>
- FishNet — What is Client-Side Prediction: <https://fish-networking.gitbook.io/docs/guides/features/prediction/what-is-client-side-prediction>
- Valve — GameNetworkingSockets `steamnetworkingtypes.h`: <https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingtypes.h> · Steam networking (same-API relink): <https://partner.steamgames.com/doc/features/multiplayer/networking> · Source Multiplayer Networking: <https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking> (403s anonymous fetches; figures re-verified in `docs/research/networking/r5-review-external.md`)
- Halo: Reach — Aldridge, *I Shot You First* (GDC 2011), summary: <https://www.wolfire.com/blog/2011/03/GDC-Session-Summary-Halo-networking>
- Tribes — Frohnmayer & Gift, *The TRIBES Engine Networking Model*: <https://www.gamedevs.org/uploads/tribes-networking-model.pdf>
- Quake 3 — Sanglard, network model review: <https://fabiensanglard.net/quake3/network.php>
- Prior in-repo research (r1-r6, verified 2026-07-22): `docs/research/networking/`
