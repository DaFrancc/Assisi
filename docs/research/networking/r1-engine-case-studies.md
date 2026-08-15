# Networking Research R1 — Engine & Game Case Studies

Research pass 1 for **Assisi** networking design. Focus: how shipped engines and
games actually implement multiplayer, extracted into patterns relevant to a
server-authoritative, snapshot-replicating ECS engine (the Assisi N0-N7 plan in
`docs/networking-design-notes.md`).

**Scope reminder / grounding facts for Assisi:** C++23, non-deterministic Jolt
build (fast-math/FMA — lockstep ruled out), custom ECS with codegen reflection
(deterministic name-sorted `ComponentId`, per-field `FieldMeta`, per-component
change ticks via `ACOMP(tracked)`), performance-first (heavy features opt-in),
Linux+Windows, 2-32 player action/co-op, listen + dedicated server. Transport
decided: GameNetworkingSockets (GNS). Model decided: server-authoritative state
replication with snapshot interpolation (Quake/Overwatch lineage).

---

## 0. The lineage in one paragraph

Assisi's chosen model is the mainstream lineage: **Tribes (1998) → Quake 3
(1999) → Source (2004) → Halo 3/Reach (2007-10) → Overwatch (2016)**, with a
parallel branch of *deterministic* engines (fighting games, RTS, Rocket League,
Photon Quantum) that Assisi deliberately does not join. The through-line of the
mainstream branch: a single authoritative server (or host) simulates the world;
it sends each client a *prioritized, partial, delta-compressed* stream of object
state over unreliable UDP with an ack/notification layer; clients interpolate
remote objects in the past and (optionally) predict their own avatar with replay
reconciliation. Every engine below is a variation on that theme, and the
variations are where the design lessons live.

---

## 1. Tribes (1998) — the ancestor everyone copied

Source: Frohnmayer & Gift, *The TRIBES Engine Networking Model*
(https://www.gamedevs.org/uploads/tribes-networking-model.pdf, archive:
https://archive.org/details/tribes-networking-model); SnapNet analysis
(http://www.snapnet.dev/blog/netcode-architectures-part-4-tribes/).

Tribes established the vocabulary the whole industry still uses. Its three
"major features" map almost 1:1 onto Assisi's plan:

1. **Multiple data-delivery requirements over one connection.** A single
   `Connection` multiplexes several managers, each with its own reliability
   need. This is exactly Assisi's *Lane* enum (`Control` reliable, `Snapshot`
   unreliable, `Bulk` reliable) — Tribes proved 27 years ago that you want one
   pipe with per-stream delivery semantics, not separate sockets.

2. **Partial object state updates (the Ghost Manager).** Objects are "ghosted"
   to a client when they come **into scope** and the ghost is deleted when they
   leave scope. State is transmitted as *partial* updates — only the fields that
   changed, not whole objects. Crucially, ghosting uses a **priority/scope**
   model, not "send everything": "available throughput is constant, so an
   increase in the number of changing objects leads to a decrease in the
   relative frequency with which objects receive updates." This is the
   **priority accumulator** idea (see Fiedler, §9) in embryonic form.

3. **Packet delivery notification protocol.** Rather than TCP-style
   retransmission, Tribes tells the higher layers *whether each packet arrived*.
   The Stream/Event/Ghost managers each get a `NotifyEvent` (delivered/dropped)
   per packet via a `TransmissionRecord`, and decide for themselves what to
   re-send. This is the seam GNS gives Assisi for free (reliable lanes) but also
   the model Assisi's snapshot delta layer implicitly needs: the server must
   know *which snapshot the client last acked* to compute the next delta.

**The Control Object / Move Manager** gives the locally-controlled player full
state + last-applied-input every packet, enabling deterministic client-side
prediction of *your own* avatar while remote objects are merely extrapolated.
This is the "local player is special" split that recurs everywhere (Unity's
"owner predicted", Overwatch's predicted-self).

**Pattern for Assisi:** the N5 "NetId map + Replicated marker + delta off change
ticks" is a re-derivation of the Ghost Manager. The one thing Assisi's plan
underweights vs. Tribes is **prioritization**: Tribes' core insight is that
bandwidth is fixed and object count is variable, so you *must* have a priority
mechanism, not just "changed → send". Assisi defers this to N7 (interest
management). That's acceptable for 2-32 players in small levels, but the
*accumulator* structure (below) is cheap to add and is the thing that keeps
"send all changed components" from blowing the packet budget when many things
move at once.

---

## 2. Quake 3 (1999) — the reference implementation of Assisi's plan

Sources: Fabien Sanglard, *Quake 3 Source Code Review: Network Model*
(https://fabiensanglard.net/quake3/network.php); Brian Hook's Q3 networking
notes (https://fabiensanglard.net/quake3/The%20Quake3%20Networking%20Mode.html).

Quake 3 is the closest published relative to Assisi's N4-N5 codec+delta design,
and it validates almost every choice:

- **Per-client snapshot ring buffer.** The server keeps **the last 32
  gamestates** it sent to each client in a cyclic array. Each new snapshot is
  computed as: copy the master gamestate, then **delta it against the most
  recent snapshot the client has acknowledged**. This is *exactly* Assisi's
  `lastAckedTick` baseline scheme in N5. Because the baseline is per-client and
  acked, packet loss degrades to "resend a bigger delta", never desync — Assisi
  states this identical property.

- **Full snapshot = delta against zero.** If no acked snapshot exists (first
  connect, or the client fell >32 snapshots behind), Q3 deltas against a "dummy
  gamestate with all fields zeroed", producing a full update through the *same
  code path*. **Lesson for Assisi:** don't build a separate "full snapshot"
  message; make the baseline nullable and let full-state fall out of the delta
  path. Assisi's plan says "baseline = last acked state per entity" but should
  explicitly make the empty baseline the spawn/late-join case.

- **Memory-introspection field diffing.** Q3 does not hand-write per-field
  compare/serialize. It uses a `netField_t` table of `(offset, bits)` and walks
  struct memory blindly, emitting **one changed-bit per field**. This is a
  hand-rolled version of exactly what Assisi's `FieldMeta` reflection gives for
  free. Assisi is strictly better positioned here — it has typed reflection, not
  just offset+bitwidth. The Q3 lesson: the *bitfield-of-changed-fields* header
  per entity is the right encoding; do that in N4's codec (a changed-mask word
  before the changed field values), not per-component-only granularity.

- **Bandwidth: 1400-byte cap + Huffman.** Q3 pre-fragments to 1400 bytes to
  dodge router MTU fragmentation and Huffman-compresses the datagram with a
  precomputed table. Assisi gets MTU handling from GNS. Huffman is a later-pass
  optimization; the bigger win (quantization) is more impactful and Assisi
  already flags it as a deferred pass.

- **playerstate vs entities.** Q3 splits the snapshot into `playerstate` (the
  receiving client's own avatar, full precision) and `entities` (everyone else).
  This is the same local-player-is-special split as Tribes' Control Object. For
  Assisi, this argues for a distinct treatment of the **owning client's
  controlled entity** even before prediction ships (N7): it wants full-rate,
  full-precision replication where other entities can be quantized/culled.

**Verdict:** N4/N5 is essentially "Quake 3 with a reflection system instead of
macros and GNS instead of a hand-rolled netchannel." This is a well-trodden,
low-risk path. The main gap is the per-*field* changed mask (Q3 has it; Assisi's
plan currently reasons at component granularity).

---

## 3. Source / Source 2 (2004+) — snapshot rates & interpolation numbers

Sources: Valve Developer Community, *Source Multiplayer Networking*
(https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking);
*Networking Entities* (https://developer.valvesoftware.com/wiki/Networking_Entities).

Source is Quake-lineage (Source began as a heavily modified Quake engine) and
supplies the concrete *tuning numbers* Assisi will need:

- **Tick vs snapshot rate are decoupled.** The server simulates at a fixed tick
  (classically 66 tick for CS:GO era, up to 128 for competitive) but sends
  **snapshots at ~20/sec by default** (`sv_updaterate`). Assisi's plan (N5)
  makes the net tick "a configurable divisor of physicsHz, start at 60". Source
  is evidence that **you do not need to snapshot at sim rate** — 20-30 Hz
  snapshot with client interpolation looks fine for most games and roughly
  *halves-to-thirds* the bandwidth vs 60 Hz. Assisi should start with a divisor
  (e.g. snapshot at 20-30 Hz over a 60 Hz sim) rather than 60 Hz, and treat
  60 Hz snapshotting as the opt-in high-fidelity mode. Godot (§7) independently
  reached the same 30 Hz default.

- **Delta snapshots + baseline.** "The server doesn't send a full world snapshot
  each time, but only changes (a delta snapshot) since the last acknowledged
  update." Full snapshots only on connect or after sustained heavy loss. Same as
  Q3, same as Assisi's plan. Confirmed convergent.

- **Client interpolation is the default remote-object story.** The client
  renders remote entities **interpolated ~100 ms in the past** (`cl_interp`,
  historically 100 ms, later `cl_interp_ratio / cl_updaterate`), buffering ~2
  snapshots. Assisi's N5 "hold ~2-3 snapshots (~100 ms), render at `serverTime -
  interpDelay`" is *literally the Source model*. Good.

- **Interest via PVS/relevancy.** "Only entities of possible interest to a
  client (visible, audible) are updated frequently." Source ties relevancy to
  the BSP PVS (potentially visible set). Assisi has no BSP; its N7 note that
  "the light-culling chunking work may share broad-phase structure" is the right
  instinct — spatial broad-phase is the relevancy oracle.

- **Lag compensation is server-side rewind.** For hitscan, the Source server
  rewinds hitboxes of all players to the attacker's view-time (reconstructed
  from the attacker's interp delay + latency) before testing the shot. This is
  the "favor the shooter" model and it's the same one Overwatch and Halo use.
  Assisi correctly defers this to N7 but must keep a **per-entity transform
  history ring** available on the server to do it — a design constraint worth
  noting now (the snapshot ring the server already keeps is a natural home).

---

## 4. Overwatch (2016) — the ECS + netcode talk that maps most directly

Sources: Tim Ford, GDC 2017 *'Overwatch' Gameplay Architecture and Netcode*
(https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and);
Edgegap deep-dive
(https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback).

This is the single most relevant case study because Overwatch is a **strict ECS**
with **server-authoritative, prediction-heavy netcode** — Assisi's exact shape.

- **ECS discipline as a netcode enabler.** Of ~150 systems, only **three** touch
  netcode (movement, weapons, state-script). Ford's framing: an architecture "so
  constrained that you are pushed toward writing consistent, maintainable,
  decoupled code almost by default" — the "pit of success." **Direct lesson for
  Assisi:** keep replication concerns in NetSync systems, and force gameplay
  systems to read `InputCommand` from `SystemContext` rather than touching input
  directly (Assisi's N3 already mandates this migration). The ECS is the asset
  here, not the obstacle.

- **Fixed simulation tick, decoupled from render.** 16 ms (60 Hz) standard,
  ~7 ms (128 Hz) tournament. "The simulation clock must be fixed even if the
  renderer isn't." Assisi's N3 `simTick` on the fixed-step accumulator is the
  same construct. Good.

- **Command frames + client clock lead.** The client runs **ahead** of the
  server by ~half-RTT + one buffered command frame (~96 ms at 160 ms RTT) so its
  inputs *arrive just before* the server needs them. Inputs are sent as a
  **sliding window (all inputs since last ack)** — redundant, unreliable,
  loss-invisible. Assisi's N3 "send last N commands per packet, unreliable,
  redundant" is exactly this. Confirmed convergent, and it's the right call.

- **Prediction + rollback for the local player.** OW predicts *all* abilities
  and projectiles by default, opting out only when necessary. On misprediction:
  roll back to the server's authoritative snapshot and **replay buffered inputs
  forward**. Assisi correctly puts this in N7 (deferred), and correctly notes
  `ReviveAt` + raw-handle EntityRef "earn their keep here" — rollback needs to
  restore prior entity state, which is what `ReviveAt` enables.

- **Time dilation for packet loss.** When the server detects input starvation it
  asks the client to simulate slightly faster (~15.2 ms instead of 16 ms) to
  rebuild the input buffer. A subtle, high-value trick Assisi can add later; not
  needed for v1 but worth a note in N7.

- **Quantized floats, not determinism.** Overwatch is *not* deterministic; it
  quantizes floats to low precision so client and server predictions don't
  rapidly diverge. This is a **direct endorsement of Assisi's non-deterministic
  + quantization plan** — you get "good enough" agreement from quantization
  without paying the determinism tax that Assisi's fast-math/FMA build can't
  afford anyway.

- **Backwards reconciliation / favor-the-shooter, with a cutoff.** Server rewinds
  the world to the shooter's frame; a **spatial bounding-volume broad-phase**
  culls hit candidates before precise tests. Beyond ~220 ms RTT prediction
  disables entirely (so a victim who dodged behind cover doesn't die). Lesson:
  lag comp needs (a) a transform history ring and (b) a broad-phase — both align
  with Assisi's existing/planned structures.

**Overwatch is the strongest evidence that Assisi's overall architecture is
sound.** The plan reads like a subset of the OW design with prediction deferred.

---

## 5. Rocket League (2015) — the *deterministic-physics* counterexample

Sources: Jared Cone, GDC 2018 *It IS Rocket Science! The Physics of Rocket
League* (slides:
https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf;
video: https://www.youtube.com/watch?v=ueEmiDM94IE); Psyonix dev tracker on
extrapolation
(https://devtrackers.gg/rocket-league/p/bb42f194...); SnapNet rollback analysis
(https://www.snapnet.dev/blog/netcode-architectures-part-2-rollback/).

Rocket League is the case that shows **why Assisi's "no lockstep" decision is
right for its game shapes but also what it gives up.** RL swapped UE3's PhysX for
**Bullet specifically to get deterministic physics** so that client and server
simulate identically and rollback works cleanly.

- **Server-authoritative + client prediction + rollback.** Clients predict the
  ball and cars; the server is authority; on divergence the client is "warped
  back" and re-simulates. Rubber-banding = a rollback where the server played
  different inputs than the client predicted.

- **Input ring buffer, redundant, decayed.** Clients send a **ring buffer of
  compressed inputs** each packet so the server can reconstruct the queue across
  loss. If the server's input buffer *empties*, it repeats the last input,
  causing "minor desyncs" — the buffer size is a latency-vs-smoothness tradeoff
  (each buffered input at 60 Hz adds ~16.6 ms; a 4-deep buffer ≈ 33-66 ms).

- **Fixed 60 Hz physics/gameplay tick, deterministic.**

**Why it wouldn't fit Assisi (and where the plan is correct):** RL's whole design
rests on *deterministic physics on both ends*. Assisi's Jolt build is
deliberately non-deterministic (fast-math, FMA, no `CROSS_PLATFORM_DETERMINISTIC`)
— so RL-style "predict the shared physics ball identically on client and server"
is **off the table**, exactly as the design notes conclude. The RL lesson Assisi
*can* borrow: the **input ring buffer with redundancy and last-input-repeat on
starvation** (already in N3), and the fact that even a physics-heavy game gets
away with server authority + client warp-back for 2-8 players. Where Assisi
differs by necessity: remote physics objects are **snapshot-interpolated ghosts
with no local Jolt body** (N5), not locally re-simulated — because Assisi can't
guarantee the local sim matches the server's. That is the correct consequence of
the non-determinism decision.

---

## 6. Halo 3 / Reach (2007-2010) — Tribes model at console scale, and "change the
game to fit the netcode"

Sources: David Aldridge, GDC 2011 *I Shot You First: Networking the Gameplay of
Halo: Reach* (Wolfire summary:
https://www.wolfire.com/blog/2011/03/GDC-Session-Summary-Halo-networking;
Edgegap: https://edgegap.com/blog/game-backend-deep-dive-halo-reach-netcode-host-migration).

Halo built directly on the **Tribes model** and is the canonical "here's how it
plays out on real hardware with real designers" talk.

- **Host-authoritative, not deterministic.** "The client asks the host for
  permission" for major actions (throw grenade → host verifies you have grenades
  → host simulates result). Client shows *cosmetic* feedback immediately
  (muzzle flash) before validation. Same authority boundary as Assisi:
  **server/host owns truth, client owns presentation.**

- **Three reliability tiers over custom UDP** (Tribes-style flow control):
  guaranteed delivery (game-over, grenade grants), guaranteed *ordering* only
  (player position), and no-guarantee (sparks, bullet holes). Maps onto Assisi's
  three Lanes; the middle tier ("ordered but not reliable" for position) is worth
  noting — GNS unreliable lanes can be configured for this.

- **"Unreliability enables aggressive prioritization."** Aldridge's key scaling
  insight, straight from Tribes: because most data is unreliable, the flow-control
  layer is free to reorder and drop, filling each packet with **the highest-
  priority state** and letting the rest converge later. Again the **priority
  accumulator** is the load-bearing mechanism.

- **Target-relative, not world-relative, aim data** to fight aiming lag — a
  domain-specific quantization trick.

- **They didn't network ragdolls.** Cutting ragdoll replication dropped their
  physics bandwidth to **~20% of Halo 3's**. The overarching lesson Aldridge
  hammers: **"change game mechanics to improve networking" rather than forcing
  netcode around inflexible design** (e.g., they tuned armor-lock timing to a
  measured RTT so mechanics *feel* aligned across latency). For Assisi: expect
  the game layer to make concessions (don't replicate every physics prop at full
  rate; derive cosmetic physics locally), and expose enough net-stat visibility
  (Assisi's N6 net-stats overlay) that designers can see the cost.

---

## 7. Godot 4 — the closest "small open-source engine" comparator

Sources: *Multiplayer in Godot 4.0: Scene Replication*
(https://godotengine.org/article/multiplayer-in-godot-4-0-scene-replication/);
proposal #3459 (https://github.com/godotengine/godot-proposals/issues/3459).

Godot 4 is the most useful *shape* comparison: a general-purpose open engine that
built a first-party high-level replication layer on a UDP library (**ENet**),
much as Assisi builds NetSync on GNS.

- **Two nodes = two concerns.** `MultiplayerSpawner` (where/what can be remotely
  instantiated) and `MultiplayerSynchronizer` (which properties sync, by which
  peer). This is a clean spawn/despawn vs. state split that mirrors Assisi's
  "spawn/despawn on reliable Control lane, state deltas on Snapshot lane."
  Godot's spawner handling of "Spawn properties auto-set on remote peers during
  spawning" is the equivalent of Assisi sending the *initial full component set*
  in the spawn message.

- **Per-property authority model.** Default authority = server; transferable per
  node via `set_multiplayer_authority()`; the classic pattern is server owns
  physics, client owns *input* for its own character (a child synchronizer with
  input authority). Same local-player split as Tribes/Quake/OW.

- **Reliability chosen per action.** Jumps use reliable RPC (discrete events);
  continuous movement uses the (unreliable) synchronizer stream. Maps to Assisi's
  Control vs Snapshot lanes.

- **Default replication tick 30 Hz**, with client interpolation; the guidance is
  explicitly "sync fewer properties, derive the rest locally (animation,
  particles), and lower the replication interval — interpolated 30 Hz looks
  identical to 60 Hz and costs far less." **This independently confirms the
  Source ~20-30 Hz snapshot lesson** and argues Assisi should not default to
  60 Hz snapshots.

- **Stated limitations:** mid-session scene changes are "problematic when players
  join mid-game," and bandwidth optimization + visibility (interest) are deferred
  to later tutorials/features. Godot shipped the *basic* replication first and
  left interest management and delta/quantization for later — **the same staging
  Assisi chose (N5 first, interest at N7).** Evidence the staging order is
  reasonable. The regret signal: late-join/scene-transition is fiddly; Assisi's
  "empty-baseline = late join" (from Q3) directly addresses the late-join half.

---

## 8. Unity Netcode — two products, two philosophies

Sources: Netcode for Entities (DOTS) *Ghost snapshots* docs
(https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/ghost-snapshots.html);
package changelog (https://github.com/needle-mirror/com.unity.netcode).

**Netcode for Entities (NfE / DOTS)** is the most architecturally similar to
Assisi (ECS-native, server-authoritative snapshots) and its **ghost** design is
worth mining:

- **Ghosts = replicated entities**, authored via a `GhostAuthoringComponent`
  prefab that declares *which fields* replicate and *how*. Assisi's analogue is
  the `Replicated{}` marker + `FieldMeta`/`serializable` flags — but Unity makes
  **per-field replication opt-in and configurable per prefab**, whereas Assisi's
  plan replicates all serializable fields of a `Replicated` component. Consider a
  per-field "replicated" flag in `FieldMeta` so, e.g., a debug/color field on a
  networked component needn't hit the wire.

- **Three ghost modes: Interpolated / Predicted / Owner-Predicted.** Remote
  ghosts interpolate in the past; the owning client's ghost is predicted; a ghost
  can be predicted-for-owner and interpolated-for-others. This is the *productized*
  form of the Tribes Control-Object split and is the exact taxonomy Assisi will
  want when N7 prediction lands. **Design forward-compat note:** Assisi should
  make "is this entity predicted on this client?" a per-connection property of the
  NetId, so the same replication path serves interpolated and predicted ghosts.

- **Delta-compressed change masks with a `Composite` flag** controlling whether a
  non-primitive field gets one change-bit for the whole struct or a bit per
  member. This is precisely the Q3 changed-bitmask idea, productized, and confirms
  Assisi should implement a **per-field change mask** in the N4 codec (currently
  the plan reasons at component granularity — this is the recurring gap).

- **Two client timelines:** interpolated (past) and predicted (present),
  presented to the rest of the sim. Same as Overwatch/Source.

**Netcode for GameObjects (NGO)** is the simpler, non-ECS product (NetworkVariables
+ RPCs, closer to Godot/UE). Its existence-alongside-NfE is itself a lesson: Unity
found that a **NetworkVariable/RPC** convenience layer and a **snapshot/ghost**
performance layer serve different audiences. Assisi's single in-engine NetSync is
fine for one game shape; don't try to be both.

---

## 9. Glenn Fiedler / Gaffer On Games — the canonical technique reference

Sources: *State Synchronization*
(https://gafferongames.com/post/state_synchronization/); *Snapshot Interpolation*
(https://gafferongames.com/post/snapshot_interpolation/); *Networked Physics*
series (https://gafferongames.com/categories/networked-physics/).

Fiedler's series is the best *concrete* source for the mechanisms Assisi's N4-N5
will actually implement. Key numbers/techniques from the state-sync article
(networking a 900-cube physics pile):

- **Priority accumulator** (the mechanism Tribes/Halo describe abstractly): each
  frame add each object's priority to a per-object accumulator, sort descending,
  fill the packet with the highest until it's full, then **reset accumulators
  only for the objects that made it in**; the rest keep accumulating and win next
  time. Objects at rest get priority bumped only when they start moving. **This is
  the single most valuable pattern Assisi's plan is currently missing** and it's
  cheap: it turns "send all changed components" into "send as many changed
  components as fit, fairly." Add it at N5 (or a light N5.5) rather than deferring
  the whole idea to N7 interest management.

- **Bandwidth-bounded packet construction:** conservatively compute remaining
  packet bytes, walk the priority-sorted list, include an object only if its
  serialized size fits. Fiedler ships **≤64 state updates/packet** while
  simulating 901 objects — i.e., you *never* try to fit the whole world in a
  packet; you fit a fair slice. Assisi's snapshot builder should be size-bounded,
  not "everything changed this tick."

- **Delta encoding against acked baseline** with a **5-bit baseline offset**
  (a ~0.5 s window at 60 Hz), falling back to absolute encoding for objects older
  than the window or freshly at-rest. Same acked-baseline scheme as Q3/Source;
  the small-offset encoding is a concrete bit-level trick for N4.

- **Quantize on both sides, every step.** Compress position (**4096 units/meter**),
  velocity, and orientation (**smallest-three quaternion, 15 bits/component**),
  and — critically — **re-quantize the whole simulation state before each step on
  both client and server** so both extrapolate from identical values, minimizing
  pops. For Assisi this matters even without prediction: if the client ever
  extrapolates a ghost, quantizing the server state it interpolates *toward* keeps
  visuals stable.

- **Jitter buffer of 4-5 frames @ 60 Hz** to normalize arrival timing — the
  concrete sizing behind Assisi's "~2-3 snapshots (~100 ms)".

- **Visual error smoothing:** keep position/orientation *error offsets* separate
  from simulated state; decay them with adaptive exponential smoothing (0.95 for
  <25 cm errors, 0.85 for >1 m) and slerp orientation error toward identity at
  0.1/frame. This is how you apply a hard authoritative correction *without a
  visible snap* — relevant to Assisi's N7 reconciliation and even to N5 when a
  late/large delta arrives.

---

## 10. Bevy (bevy_replicon / lightyear), Flecs, O3DE — ECS-native replication

### bevy_replicon
Sources: repo (https://github.com/projectharmonia/bevy_replicon);
docs (https://docs.rs/bevy_replicon).

Server-authoritative replication crate. Key design points relevant to Assisi:

- **Uses the ECS's built-in change detection** to decide what to send —
  *exactly* Assisi's plan of driving deltas off `ACOMP(tracked)` change ticks.
  This validates the approach and the dependency on solid change detection.
- **It "does not distinguish between modification and re-insertion" — it sends the
  list of changes and lets the client decide how to apply.** A pragmatic
  simplification worth copying.
- Transport-agnostic (pairs with `renet`/others), just as Assisi splits `Net`
  (transport) from `NetSync` (replication). The two-module split is the standard
  ECS-net shape.
- Rollback/prediction is a *separate* crate (`bevy_timewarp`) layered on top —
  reinforcing Assisi's decision to defer prediction (N7) as an additive module,
  not bake it into the core replication path.

**The load-bearing caveat for Assisi:** replicon leans entirely on Bevy's *robust*
change detection. Assisi's design notes flag exactly the risk here — the
**"change-tick landmine"**: `Query` yields mutable refs without stamping, so a
system mutating a tracked component via a query produces *no delta* and causes
silent desync. This is the #1 correctness prerequisite. Bevy avoids it because
Bevy's `Mut<T>` deref *always* stamps a change tick. **Recommendation: adopt
Bevy's model — make mutable access stamp automatically (design-notes option (a),
the stamping query variant) rather than relying on GetMut/MarkChanged discipline
(option (b)).** Every ECS-net system that works relies on automatic change
stamping; discipline-based approaches rot.

### lightyear
Sources: repo (https://github.com/cBournhonesque/lightyear).

The "batteries-included" Bevy netcode lib: client-side prediction, rollback,
snapshot interpolation, WASM transports. Its component model is instructive:
`ReplicationSender`/`ReplicationReceiver` mark a peer as replicating;
`PredictionManager`/`InterpolationManager` mark entities as predicted/interpolated
— i.e., **prediction and interpolation are per-entity, opt-in, composable
markers.** This is the same forward-compat structure recommended in §8: make
"predicted vs interpolated" a per-entity/per-connection property so N7 prediction
slots onto the N5 replication path without a rewrite.

### Flecs
Flecs itself ships no first-party netcode; its users build replication on top of
its reflection/serialization (Flecs has strong runtime reflection). The pattern:
Flecs users serialize components via the reflection system and diff against a
snapshot — the **exact** pattern Assisi gets from `FieldMeta`. Confirms
reflection-driven serialization is the standard ECS-net foundation.

### O3DE — AzNetworking + Multiplayer Gem
Sources: Multiplayer Gem overview
(https://docs.o3de.org/docs/user-guide/gems/reference/multiplayer/multiplayer-gem/overview/);
entity-hierarchies RFC
(https://github.com/o3de/sig-network/blob/main/rfcs/rfc-net-20211005-2-entityhierarchies.md);
Networking overview (https://docs.o3de.org/docs/user-guide/networking/).

O3DE (open-source, ex-Amazon Lumberyard/CryEngine lineage) is the closest
**production-grade open C++ ECS engine with first-party netcode** — the single
best structural comparator for Assisi:

- **Two layers, mirroring Assisi's Net/NetSync split.** `AzNetworking` is the
  low-level transport ("reduced code size, low latency on send/recv, low message
  overhead") providing **reliable UDP with acks**; the **Multiplayer Gem** is the
  higher entity-replication layer on top. This is *precisely* Assisi's
  `Assisi::Net` (dumb pipe) + `Assisi::NetSync` (game protocol) layering.
  Independent convergence on the same architecture is strong validation.

- **Server delta against per-client acked state, every frame.** "A server checks
  for a delta on every frame by inspecting a snapshot of the world state against
  each client's acknowledged state; if different from the last state that resulted
  in an ACK, it generates a delta for that client and transmits it." This is
  Quake 3 / Assisi's N5 baseline-delta scheme, restated for an ECS. Confirmed
  convergent.

- **Push-based `NetBindComponent` + NetworkProperties + RPCs.** Components declare
  *network properties* (replicated state) and RPCs (events). `NetBindComponent` is
  the per-entity hub; its `ProcessInput` runs on the client (on input) and on the
  server (applying input) — the same input-command flow as Assisi's N3
  `InputCommand`. O3DE is **push-based** (components notify on change) rather than
  polled — same conclusion Unreal reached with Iris (§11) and Assisi reaches with
  change-ticks.

- **Entity hierarchies are a hard problem.** O3DE dedicated an RFC to networked
  parent/child entities (players in vehicles, attachments) — a reminder that
  **hierarchical/attached entities are a known sharp edge** Assisi's flat-NetId
  plan will eventually hit (e.g., a player riding a server-owned platform). Not a
  v1 concern but worth a design-notes line.

---

## 11. Unreal Engine — the most-used system, and its Iris rewrite (what the
incumbents regret)

Sources: Iris docs
(https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine,
https://dev.epicgames.com/documentation/en-us/unreal-engine/components-of-iris-in-unreal-engine);
Vorixo, *The magic of Network Managers*
(https://vorixo.github.io/devtricks/network-managers/); Fast TArray write-ups
(https://unrealution.com/optimization/understanding-fast-tarray-replication...).

Unreal's *legacy* replication (property replication + RPCs + actor channels) is
the industry's most battle-tested system, and the **reasons Epic rewrote it as
Iris** are the most valuable "regrets" data available:

- **Legacy is poll-based and that's the bottleneck.** Default replication polls
  every replicated property of every relevant actor each `NetBroadcastTick` to
  detect changes. Community wisdom (Vorixo): **"CPU timing issues from bad
  NetBroadcastTick metrics are far more common than bandwidth issues."**
  Replicating a few hundred moving actors while *thousands* of non-moving
  replicated actors clutter the poll loop is the classic UE perf cliff. **Lesson
  for Assisi: do NOT poll. Assisi's change-tick model is already push-ish — the
  server iterates `Changed<T>(entity, lastAckedTick)` rather than diffing every
  component every tick. Keep it that way; the change-tick landmine (§10) is what
  stands between "push" and "accidentally poll-equivalent."**

- **Iris is explicitly push-model.** Iris's stated goals: "larger, more
  interactive worlds, higher player counts, lower server costs," built on
  Fortnite BR experience. Objects **notify the system of modifications rather than
  the system polling** (the docs are explicit: "State Tracking: objects notify the
  system of modifications rather than the system polling for changes"). Iris
  components: `ReplicationBridge` (lifetime + NetRefHandles = stable net ids),
  `ReplicationStates` (structs with an embedded **dirty bitfield per member** —
  the change mask again), `NetSerializers` (transform + **quantize** + delta),
  and a `ReplicationSystem` API for **filtering (interest), prioritization,
  groups, RPCs.** Iris's shape is a superset of Assisi's plan; the parts Assisi
  defers (filtering/prioritization groups) are first-class in Iris precisely
  because Epic learned they're needed at scale.

- **Net Dormancy is the legacy band-aid for the poll cost** — actors go "dormant"
  to drop out of the replication loop until dirtied. But dormancy "results in less
  net-responsive actors and complicated relevancy scenarios" (Vorixo), and
  reduced `NetUpdateFrequency` + dormancy interacts badly with priority. Assisi's
  change-tick model gets dormancy *for free* (an unchanged entity produces no
  delta, costs nothing) **without** the responsiveness cliff — a genuine
  architectural advantage over legacy UE, provided change detection is correct.

- **Relevancy & the Replication Graph.** UE's `AActor::IsNetRelevant` + the
  **Replication Graph** (spatial grid of nodes, so the server evaluates far fewer
  actors per client) is the production answer to interest management. It confirms
  Assisi's N7 instinct that interest management should be **spatial broad-phase
  driven** (share the light-culling chunking). The Replication Graph exists
  because per-actor relevancy checks don't scale — Assisi should plan for a
  grid/chunk relevancy structure, not per-entity distance checks, when N7 lands.

- **Fast TArray Replication** (delta-replicated dynamic arrays) exists because
  naive array replication resends the whole array; its **caveat** is instructive:
  "you must mark items dirty in game code, and list order isn't guaranteed
  identical between client and server." I.e., even Epic's optimized path pushes
  *manual dirty-marking* onto gameplay code — the same discipline-vs-automatic
  tension as Assisi's change-tick landmine. Automatic stamping avoids this class
  of bug.

- **RPCs vs property replication.** UE offers both reliable/unreliable RPCs
  (events) and property replication (state). The enduring lesson from the UE
  community: **use property/state replication for anything that has a "current
  value"; use RPCs only for genuine one-shot events**, because RPCs don't
  self-heal on loss the way acked-baseline state does. Assisi's plan (state via
  snapshots; discrete spawn/despawn + control on reliable lane) already honors
  this, but the principle is worth codifying: resist the urge to add an "event
  RPC" for anything that is really replicated state.

---

## 12. Cross-cutting patterns (the synthesis)

**Where the authority boundary goes.** Unanimous across all case studies:
**server/host simulates truth; client owns presentation + its own input.** The
only variation is how much the client *predicts* (nothing in Godot-basic; local
avatar in Q3/Source/Tribes; nearly everything in Overwatch; the full shared
physics sim in Rocket League — enabled only by determinism Assisi lacks). Assisi
sits correctly at "server owns everything incl. physics; client interpolates
ghosts; local prediction is a deferred additive module."

**Tick model.** Everyone runs a **fixed simulation tick decoupled from render**
(Overwatch 60/128, RL 60, Source 66/128). **Snapshot/replication rate is lower
than sim rate** (Source ~20, Godot 30) with client interpolation covering the
gap. Assisi should default snapshots to a *divisor* of the 60 Hz sim (20-30 Hz),
not 60 Hz.

**Spawn/despawn.** The consistent pattern: a **stable, server-allocated network
id** distinct from local handles (Tribes ghost id, Q3 entity number, UE
NetRefHandle/NetGUID, O3DE net entity id) — Assisi's `NetId` is exactly this and
is non-negotiable (local `(index,generation)` handles are not cross-machine
stable). Spawn = reliable message carrying the initial full component set;
despawn = reliable message; state = unreliable deltas. Godot's Spawner/Synchronizer
split and Halo's reliability tiers both encode this. **Make the empty baseline =
spawn/late-join** (Q3) so late joiners reuse the delta path.

**Relevancy / interest.** Every engine that scaled past a few players has interest
management, and it's **always spatial broad-phase driven** (Source PVS, UE
Replication Graph grid, Overwatch bounding-volume culling, Tribes scope). Assisi
defers it (N7) and correctly plans to reuse the light-culling chunk structure.
For 2-32 players in small levels, deferral is fine — but the **priority
accumulator** (§9) is a *cheaper, earlier* mechanism that bounds packet size
before full interest management exists, and it's the piece the plan is missing.

**Physics objects.** The determinism split governs everything. Deterministic
engines (RL, Quantum) re-simulate physics on both ends and rollback. Non-
deterministic engines (Overwatch, Source, Halo, Assisi) **do not** — remote
physics objects are snapshot-interpolated ghosts with the server as sole
authority, and cosmetic physics (ragdolls, debris, particles) is **derived
locally and not replicated** (Halo's ragdoll cut → 20% bandwidth). Assisi's N5
"remote entities have no local Jolt bodies, kinematic ghosts at most; transient
components never on the wire, rebuilt locally" is exactly correct and directly
supported by Halo/Overwatch evidence.

**Bandwidth techniques (in rough priority order of ROI):**
1. **Delta against per-client acked baseline** (Q3/Source/O3DE/Fiedler) — Assisi
   has this (N5).
2. **Per-field change mask** (Q3 netField bits, Unity Composite flag, Iris dirty
   bitfield) — Assisi's plan under-specifies this at *field* granularity; add it.
3. **Priority accumulator + size-bounded packets** (Tribes/Halo/Fiedler) —
   **missing from the plan; add at N5.**
4. **Quantization** (Overwatch low-precision floats, Fiedler 4096/m + smallest-
   three 15-bit quats) — Assisi defers (fine), but note quantize-both-sides to
   stabilize interpolation.
5. **Interest management** (spatial) — deferred to N7, fine for target scale.
6. **Huffman/entropy coding of the datagram** (Q3) — lowest ROI, defer
   indefinitely; GNS handles MTU.

**The determinism decision.** Rocket League and Photon Quantum are the
"what-if-we-went-deterministic" branch. Quantum requires **fixed-point math (FP
struct replacing all float/double)** and forbids any float→fixed conversion
inside the sim ("causes desyncs 100% of the time"). This is fundamentally
incompatible with Assisi's fast-math/FMA/non-deterministic-Jolt build — you
cannot have both `-ffast-math` performance and lockstep determinism. **Assisi's
"state replication, never lockstep" decision is correct and well-evidenced**;
the cost is only that shared-physics prediction (RL-style) is unavailable, which
for 2-32 player action/co-op is an acceptable trade (Overwatch/Source/Halo all
made the same trade and shipped fine).

---

## Implications for Assisi — opinionated takeaways

**What the N0-N7 plan gets right (strongly evidence-backed):**

1. **Server-authoritative state replication + snapshot interpolation, no
   lockstep.** Directly matches Overwatch/Source/Halo; the non-determinism of the
   Jolt build makes it the *only* correct choice, and Quantum/RL confirm the
   alternative's cost is fixed-point-everything, which is off the table.
2. **Net/NetSync two-module split.** Independently reinvented by O3DE
   (AzNetworking + Multiplayer Gem) and Bevy (transport crate + replicon). This is
   *the* standard ECS-net layering. Keep it.
3. **NetId distinct from local `(index,generation)` handles.** Universal
   (Tribes ghost id, Q3 entity num, UE NetRefHandle, O3DE net id). Non-negotiable,
   correctly specified.
4. **Per-client acked-baseline delta off change ticks.** Q3/Source/O3DE/replicon
   all do exactly this. The single most-validated part of the plan.
5. **Reflection-driven binary codec (FieldMeta).** Q3 hand-rolled this with
   offset tables; Flecs/replicon users do it with reflection. Assisi's typed
   reflection is *better* raw material. Right call.
6. **Input as redundant unreliable ring buffer, sampled once per fixed tick.**
   Matches Overwatch's sliding window and Rocket League's input ring exactly.
7. **Remote physics = interpolated ghosts, no local Jolt body; transients rebuilt
   locally.** Directly supported by Halo (ragdoll cut) and Overwatch.
8. **~100 ms / 2-3 snapshot interpolation buffer.** Literally the Source
   `cl_interp` default; Fiedler's 4-5 frame jitter buffer confirms the sizing.
9. **Deferring prediction/rollback and lag-comp to N7 as additive modules.**
   Bevy (timewarp separate crate) and lightyear (per-entity prediction markers)
   confirm prediction should layer on top, not be baked in.
10. **Headless server refactor before protocol work (N2).** Matches every
    dedicated-server architecture; independently valuable.

**What the plan under-weights or should change:**

1. **The change-tick landmine is THE make-or-break item — resolve it toward
   automatic stamping (option a), not discipline (option b).** Every working
   ECS-net system (Bevy/replicon, Iris, O3DE) relies on *automatic* change
   detection; every system that pushes manual dirty-marking onto gameplay code
   (UE Fast TArray) documents it as a footgun. Discipline-based dirty tracking
   rots the moment a new gameplay system forgets `GetMut`. This is not a "decide
   at implementation time" item — the evidence says pick automatic stamping now.

2. **Add a priority accumulator + size-bounded snapshot packets at N5, don't wait
   for N7 interest management.** This is the load-bearing scaling mechanism in
   Tribes, Halo, and Fiedler, and it's cheap (a per-object accumulator + a
   sorted fill loop). Without it, "send all changed components this tick" can
   blow the packet budget the moment many entities move at once — the exact
   failure Tribes designed around. It also gracefully degrades before interest
   management exists.

3. **Specify the change mask at *field* granularity, not just component
   granularity.** Q3 (netField bits), Unity (Composite flag), and Iris (per-member
   dirty bitfield) all encode *which fields* changed within a component. Assisi's
   N4 currently reads as "component changed → send the component." A per-field
   changed-mask word before the field values is a large, cheap bandwidth win and
   is trivial given `FieldMeta`.

4. **Default snapshots to a divisor of sim rate (20-30 Hz), not 60 Hz.** Source
   (20) and Godot (30) both default well below sim rate with interpolation, for
   roughly 2-3x bandwidth savings at no visible cost. The plan's "start 60, make
   configurable" (open decision #2) should start ~20-30.

5. **Make full-state fall out of the empty-baseline delta path (Q3), and treat
   late-join as `baseline = null`.** The plan mentions "baseline = last acked
   state" but should explicitly unify spawn / late-join / recovery-from-loss into
   one delta-against-empty codepath. Godot's stated late-join pain is the warning.

6. **Design NetId now to carry a per-connection "predicted vs interpolated"
   bit.** Unity (owner-predicted mode) and lightyear (per-entity prediction
   markers) show prediction must be a per-entity/per-connection property so N7
   slots onto the N5 path without a rewrite. Reserve the concept in the NetId map
   even though prediction is deferred.

7. **Keep a server-side per-entity transform history ring from N5 on.** Lag
   compensation (N7) needs it, and the snapshot ring the server already keeps for
   deltas is the natural home. Provision it early so N7 isn't a data-model change.

8. **Note hierarchical/attached entities as a known future sharp edge.** O3DE
   needed a dedicated RFC for networked parent/child (players in vehicles/on
   platforms). Assisi's flat NetId is fine for v1 but will hit this; a design-notes
   line now saves a surprise later.

9. **Don't add "event RPCs" for anything that is really replicated state.** The
   enduring UE lesson: state self-heals under loss via acked baselines; RPCs
   don't. The plan is already state-first — codify the rule so it stays that way.

**Bottom line:** the N0-N7 plan is a faithful, low-risk re-derivation of the
Quake 3 → Overwatch lineage, and independent convergence with O3DE and Bevy on
the two-module + change-detection-driven-delta shape is strong validation. The
two substantive gaps are (a) **automatic change stamping** (resolve the landmine
toward option a) and (b) **prioritized, size-bounded snapshot packets** (pull the
priority accumulator forward from N7 into N5). Add per-field change masks and a
lower default snapshot rate and the design is squarely in line with everything
that has shipped.

---

### Source index (primary/high-value)

- Tribes Networking Model — https://www.gamedevs.org/uploads/tribes-networking-model.pdf ; https://archive.org/details/tribes-networking-model
- SnapNet, Netcode Architectures (Tribes / Rollback) — http://www.snapnet.dev/blog/netcode-architectures-part-4-tribes/ ; https://www.snapnet.dev/blog/netcode-architectures-part-2-rollback/
- Quake 3 Network Model (Sanglard) — https://fabiensanglard.net/quake3/network.php
- Source Multiplayer Networking (Valve) — https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking ; https://developer.valvesoftware.com/wiki/Networking_Entities
- Overwatch Gameplay Architecture & Netcode (Ford, GDC 2017) — https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and ; https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback
- Rocket League "It IS Rocket Science" (Cone, GDC 2018) — https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf ; https://www.youtube.com/watch?v=ueEmiDM94IE
- Halo Reach "I Shot You First" (Aldridge, GDC 2011) — https://www.wolfire.com/blog/2011/03/GDC-Session-Summary-Halo-networking ; https://edgegap.com/blog/game-backend-deep-dive-halo-reach-netcode-host-migration
- Godot 4 Scene Replication — https://godotengine.org/article/multiplayer-in-godot-4-0-scene-replication/
- Unity Netcode for Entities, Ghost Snapshots — https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/ghost-snapshots.html
- Photon Quantum (determinism / fixed point) — https://doc.photonengine.com/quantum/current/manual/quantum-ecs/fixed-point ; https://doc.photonengine.com/quantum/v3/quantum-intro
- Gaffer On Games — https://gafferongames.com/post/state_synchronization/ ; https://gafferongames.com/post/snapshot_interpolation/ ; https://gafferongames.com/categories/networked-physics/
- bevy_replicon — https://github.com/projectharmonia/bevy_replicon ; lightyear — https://github.com/cBournhonesque/lightyear
- O3DE Multiplayer Gem / AzNetworking — https://docs.o3de.org/docs/user-guide/gems/reference/multiplayer/multiplayer-gem/overview/ ; https://github.com/o3de/sig-network/blob/main/rfcs/rfc-net-20211005-2-entityhierarchies.md
- Unreal Iris — https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine ; https://dev.epicgames.com/documentation/en-us/unreal-engine/components-of-iris-in-unreal-engine ; Vorixo Network Managers — https://vorixo.github.io/devtricks/network-managers/
