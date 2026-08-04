# R3 — Replication-Layer Architecture: Modern Theory & Practice, Grounded in Assisi

*Research round 3. Scope: the replication layer **above** transport — snapshot/state models, bit-level
codecs, prediction & reconciliation for non-deterministic physics, ECS-native replication, tick/clock
design. Grounded in the Assisi ECS (`modules/ECS`, `modules/Core/.../Reflect`) and evaluated against
`docs/networking-design-notes.md` stages N0–N7. Ends with an opinionated verdict on N3–N7.*

---

## 0. Framing: what the target actually is

Assisi targets **2–32 players**, server-authoritative, snapshot-interpolation lineage
(Quake→Source→Overwatch), explicitly **not** deterministic lockstep because the build ships
`-ffast-math` + AVX2/FMA and Jolt without `CROSS_PLATFORM_DETERMINISTIC`. That single constraint
decides most of the design space up front: **lockstep and deterministic full-sim rollback are off the
table**, and everything downstream is a variation on "server simulates the truth, clients render a
delayed/interpolated copy, the local player optionally predicts."

The good news is that at 2–32 players almost every hard scaling problem in the literature
(replication graphs, PVS, aggressive interest management, the entire reason Tribes exists) is a
*non-problem*. The design should be honest about that and not import machinery it will never need. The
research below repeatedly lands on the same conclusion: **a Quake-3-style delta-snapshot model with a
priority accumulator escape hatch is the right center of gravity**, and the interesting engineering is
in (a) the FieldMeta-driven codec, (b) the change-tick plumbing, and (c) prediction of *only* the local
controller.

---

## 1. Snapshot replication canon

### 1.1 Quake 3: delta against an acked baseline

Quake 3 is the canonical delta-snapshot model and still the cleanest one to copy. The server keeps a
ring of recently-sent snapshots per client; each outgoing snapshot is **delta-compressed against the
most recent snapshot the client has acknowledged**, not against the immediately previous one
([Fabien Sanglard's Q3 network review](https://fabiensanglard.net/quake3/network.php),
[jfedor Q3 protocol](https://www.jfedor.org/quake3/),
[bookofhook Q3 networking](http://trac.bookofhook.com/bookofhook/trac.cgi/wiki/Quake3Networking)).
The acked baseline is usually a few packets old — "the newest one the server *knows* the client has"
— so under loss the delta simply gets fatter (more fields differ) rather than desyncing.

Key mechanics worth stealing verbatim:

- **Field-level change bits.** `playerState`/`entityState` are fixed field lists. For each field the
  server compares old-vs-new; unchanged fields cost **one bit** ("no change"), changed fields cost
  one bit + the value. This is delta-against-baseline at *field* granularity, and it is why Q3
  snapshots are tiny.
- **Entity presence.** Entities that exist in the new frame but not the baseline are "new" (send full);
  entities in baseline but not new frame get an explicit "remove" bit; entities in both get the field
  delta. This is the spawn/despawn protocol falling out of the delta for free.
- **Ack is the sequence number.** The client's outgoing packets carry the last snapshot sequence it
  received; the server reads that to pick the baseline. No separate reliable ack channel needed for
  state — the state channel is self-acking.

How many baselines to keep: enough to cover the client's outstanding-unacked window, i.e. roughly
`RTT × snapshot_rate` snapshots, with a hard cap. Q3 kept 32 (`PACKET_BACKUP`). At 30–60 Hz and
LAN-to-broadband RTTs, 32–64 per client is plenty; if the client hasn't acked within that window you
force a **full (baseline-less) snapshot** and resync. This is the "degrade to resend more, never
desync" property the Assisi plan already articulates in Stage 5.

### 1.2 Delta-against-acked vs delta-against-last-sent

- **Against last-sent** (simplest): tiny deltas, but a *single* lost packet corrupts the client's
  baseline and every subsequent delta is wrong until a keyframe. Unusable on unreliable transport
  unless you add reliable snapshot delivery (which reintroduces head-of-line blocking).
- **Against last-**acked** (Quake/Source/Fiedler): the server only ever deltas against state the client
  provably has. Loss costs bandwidth (bigger deltas until the ack catches up), never correctness. This
  is the industry default and what Assisi should do.

Glenn Fiedler's *State Synchronization* article reaches the same design from the physics side: "the
steady state becomes the sender encoding snapshots relative to a baseline that is roughly RTT in the
past," with per-object 5-bit references to the acked sequence
([Gaffer, State Synchronization](https://gafferongames.com/post/state_synchronization/)).

### 1.3 Source engine: interpolation + the 100 ms lerp

Source formalizes the client-side half. Clients render **in the past** by a fixed interpolation delay
(`cl_interp`, default **100 ms** = 2 snapshots at 20 Hz) and interpolate remote entities between the two
snapshots straddling `renderTime = serverTime − interpDelay`
([Source Multiplayer Networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking),
[Valve Interpolation](https://developer.valvesoftware.com/wiki/Interpolation)). The server *knows* each
client's interp amount and factors it into lag compensation (below). This is exactly the model in
Assisi's Stage 5 client ("hold ~2–3 snapshots (~100 ms), interpolate between the two straddling
`serverTime − interpDelay`"). Nothing to change there — it is the correct, boring answer.

### 1.4 When does pure snapshot break down?

Snapshot delta breaks down on **entity count × update rate × player count**, because the server does
`O(entities × clients)` field comparisons per net tick and bandwidth scales with churned-entity count.
The failure mode is CPU (the diff) before bandwidth at small scale. At 2–32 players with, say, low
thousands of replicated entities, this is comfortably fine on one core — this is precisely the regime
Quake 3 and Source ship in. The escape valve if a scene ever gets large is the **priority accumulator**
(§2.2), which caps per-packet work regardless of entity count. Assisi should build the plain full-diff
first and keep the accumulator as a documented, non-blocking upgrade seam.

---

## 2. The eventual-consistency alternative (Tribes) and modern priority designs

### 2.1 Tribes: ghosts, scope, eventual consistency

The Tribes model ([Frohnmayer & Gift, *The TRIBES Engine Networking Model*](https://www.gamedevs.org/uploads/tribes-networking-model.pdf),
[SnapNet Part 4](http://www.snapnet.dev/blog/netcode-architectures-part-4-tribes/)) is the other pole.
Instead of coherent whole-world snapshots, each object is a **ghost** with a **priority** and a
**state mask** (dirty bits per property). The Ghost Manager packs *whatever fits* into each packet,
highest-priority-first, and guarantees only **eventual consistency**: a ghost's state will *eventually*
match the server "provided the server makes no further changes." Intermediate states can be skipped;
there is no globally coherent timestamp across objects. Objects enter/leave a client's **scope**
(interest set) and their ghosts are created/destroyed accordingly. Five stream managers (events, ghosts,
moves, datablocks, strings) share the packet in fixed lanes so urgent data never starves.

Tribes' whole reason for existing is **scale under a bandwidth cap** — 128 players over a 28.8k modem
in 1998. SnapNet is blunt: "for games with a large enough scale or in bandwidth-constrained
environments, the Tribes architecture is typically one of the only viable models," but it comes "at
significant cost — requiring defensive gameplay coding against inconsistent states the netcode itself
introduces." That last clause is the killer for Assisi: eventual consistency pushes complexity *into
every gameplay system*, which fights the engine's performance-first, keep-it-simple ethos.

### 2.2 The priority accumulator (the good idea to steal)

The reusable jewel from Tribes/Fiedler is the **priority accumulator**: a `float` per replicated object,
accumulated every tick by the object's priority, sorted descending, and the top-N drained into the
packet up to a byte budget; drained objects reset, un-sent objects **keep** their accumulator so they
rise and eventually get sent (anti-starvation) ([Gaffer, State Synchronization](https://gafferongames.com/post/state_synchronization/)).
This is *precisely* how Unreal computes actor net priority: `GetNetPriority()` multiplies a base
`NetPriority` by **time-since-last-replicated**, so starved actors climb
([Unreal actor relevancy & priority](https://cedric-neukirchen.net/docs/multiplayer-compendium/actor-relevancy-and-priority/),
[Matt Gibson on UE replication](https://www.mattgibson.dev/blog/unreal-replication-settings)).

The accumulator is a **strict superset of the full-diff snapshot**: with an infinite byte budget it sends
everything changed every tick (= plain snapshot); with a finite budget it gracefully degrades. So the
right architecture is: build the full delta-snapshot now, but structure the server send loop so the
"which changed entities go in this packet" step is a sortable list, not a hardcoded for-loop. Then the
accumulator drops in later with zero protocol change.

### 2.3 Unreal replication graph / dormancy (what NOT to build yet)

Unreal's Replication Graph exists to answer "which of **50,000** actors are relevant to each of **100**
clients" without an `O(actors×clients)` relevancy scan, by bucketing actors into spatial/always-relevant
nodes ([Epic: Replication Graph overview](https://www.unrealengine.com/en-US/tech-blog/replication-graph-overview-and-proper-replication-methods),
[vorixo network managers](https://vorixo.github.io/devtricks/network-managers/)). **Dormancy**
(`FlushNetDormancy`) is the push-model optimization: an actor that isn't changing is marked dormant and
skipped entirely until something wakes it ([hzFishy dormancy & relevancy](https://notes.hzfishy.fr/Unreal-Engine/Networking/Core/Dormancy-and-relevancy)).

At 2–32 players these are premature. But **dormancy's principle is exactly Assisi's change-tick model**:
don't even consider an entity whose components haven't changed since the client's acked tick. Assisi
gets dormancy "for free" from change ticks (§5.1) without the actor-graph machinery. The lesson to bank:
*change detection is the poor man's replication graph, and at this scale it's sufficient.*

---

## 3. Bit-level encoding: quantization, bitpacking, and the FieldMeta codec

### 3.1 What real engines actually put on the wire

Nobody ships raw protobuf/flatbuffers as the *snapshot* payload in a twitch game. The consensus, from
Fiedler through Q3 through Source, is a **hand-rolled bitpacker** over quantized, delta-encoded fields.
The reasons:

- **Protobuf** varint-encodes and tag-prefixes every field and requires an encode/decode step into a
  parallel message representation — "expensive in both processing power and memory allocation … done
  frequently such as network server communication" ([FlatBuffers docs](https://flatbuffers.dev/),
  [Protobuf vs FlatBuffers for real-time](https://www.oreateai.com/blog/protobuf-vs-flatbuffers-choosing-the-right-serialization-framework-for-realtime-applications/f410bfacd86a9235f73cf2747c5026aa)).
  Byte-aligned, so it can't express "1 bit = unchanged."
- **FlatBuffers** wins on *zero-copy random access* to large structured blobs (good for asset/config
  streaming) but is byte-aligned and carries a vtable per object — wrong tool for a dense snapshot of
  small components.
- **Hand-rolled bitpacking** trades a little CPU on read/write for **sub-byte fields** — the "1 bit for
  unchanged" trick is impossible without it, and it's the single biggest bandwidth win
  ([Gaffer, Reading and Writing Packets](https://gafferongames.com/post/reading_and_writing_packets/),
  [Gaffer, Serialization Strategies](https://gafferongames.com/post/serialization_strategies/),
  [KinematicSoup, Bit-Packing 101](https://kinematicsoup.com/news/2016/9/6/data-compression-bit-packing-101)).

Verdict for Assisi: **protobuf is already in the tree (GNS dependency) but must not touch the snapshot
path.** The snapshot codec is a hand-rolled `BitWriter`/`BitReader`. (The plan's Stage 4 says
`ByteWriter`/`ByteReader`, byte-aligned LE — see §7 for why that should become bit-level, or at least
bit-capable, sooner than "later.")

### 3.2 Quantization numbers worth copying

Concrete, battle-tested figures from Fiedler's *Snapshot Compression*
([Gaffer](https://gafferongames.com/post/snapshot_compression/)):

- **Position**: bound the world (e.g. ±256 m XY, 0–32 m Z), quantize to 512 units/m (~2 mm). ⇒ 18/18/14
  bits = **50 bits** absolute (from 96). Delta-encoded (small-range indicator + 5 or 9 bits/component)
  averages **~26 bits**.
- **Orientation, smallest-three**: a unit quaternion has `x²+y²+z²+w²=1`, so drop the **largest**
  component (best precision, since remaining three are ≤ 0.707) and send 2 bits for "which was largest"
  + 3 × 9 bits = **29 bits** absolute (from 128); reconstruct the dropped one via
  `sqrt(1−a²−b²−c²)`, always non-negative since `q` and `−q` are the same rotation
  ([StagPoint smallest-three gist](https://gist.github.com/StagPoint/bb7edf61c2e97ce54e3e4561627f6582),
  [Unity QuaternionCompressor](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@1.5/api/Unity.Netcode.QuaternionCompressor.html)).
  Delta-encoded ~23 bits.
- **Velocity**: only send when non-zero; at rest, one bit. (Fiedler omits `(0,0,0)` entirely.)

Aggregate result: Fiedler's 900-cube demo went from **17.37 Mbps** (raw 60 Hz) to **~256 kbps** — 1.5%.
The two multipliers were quantization and delta-against-baseline; smallest-three and per-field change
bits are the rest.

**Critical correctness rule** (Fiedler, State Sync): if the *server* also runs physics and you predict,
**quantize the state on both sides identically** — feed the quantized values back into the simulation so
client and server extrapolate from the same numbers, or quantization error accumulates into visible pops.
For Assisi's v1 (remote entities are non-predicted ghosts) this matters less, but the codec should be
written so quantize/dequantize is a pure function reusable on both ends.

### 3.3 Encoding primitives

- **Varint + zigzag** for unbounded integers (entity counts, NetIds, tick deltas): LEB128 varint for
  magnitude, zigzag (`(n<<1)^(n>>31)`) to fold signed into unsigned so small negatives stay small. Good
  default for *counts and ids*; for *quantized physical quantities* a fixed bit-width beats varint
  because you already know the range.
- **Delta per field** against the acked baseline value, not just delta-presence: e.g. position delta in
  a small range costs far less than the absolute.
- **Change bitmask per component**: a leading bitmask (one bit per field) says which fields follow. This
  is the FieldMeta-native generalization of Q3's per-field change bit.

### 3.4 FieldMeta-driven codec design

Assisi's reflection (`modules/Core/include/Assisi/Core/Reflect/FieldMeta.hpp`) already carries everything
a generic codec needs: `FieldType` (Float/Vec3/Quat/Mat4/Enum/EntityRef/AssetId/String/…), byte
`offset`, `transient` flag, and — importantly — `hasMin/hasMax/minValue/maxValue` and `enumSize/enumSigned`.
`ComponentMeta` carries `name`, ordered `fields`, `serializable`, `tracksChanges`, and the dense
alphabetical `id`. `ComponentRegistry` gives deterministic name-sorted ids on both ends. This is a
*better* starting point than most engines have; the codec should exploit it:

1. **Field ordering is already deterministic** (declaration order within a component; components in
   alphabetical id order). Both ends walk `ComponentMeta::fields` in the same order — no field tags on
   the wire needed. This is the flatbuffers/protobuf tax Assisi gets to skip.

2. **Per-type encoders keyed on `FieldType`.** A dispatch table `FieldType → {encode, decode, bits}`.
   `Float` → quantized if the field has `hasMin/hasMax` (use the range!), else raw 32-bit. `Quat` →
   smallest-three. `Vec3` for position → bounded quantize; for velocity → rest-detection + quantize.
   `Enum` → `enumSize`-driven, `ceil(log2(enumConstants.size()))` bits if you want to get fancy, else
   `enumSize` bytes. `EntityRef` → **NetId** (§5.2), never the local `(index,generation)`. `String`/
   `AssetPath` → length varint + bytes (rare on the snapshot path; prefer `AssetId` GUIDs). **The
   `min/max` editor hints become quantization ranges — that's a free, already-authored bit budget.**

3. **The change bitmask.** Prepend a `fields.size()`-bit mask; only masked fields follow. For a delta
   snapshot, compute the mask by comparing live component to the client's acked baseline component. For
   a spawn (no baseline), mask = all-ones (full component). This unifies spawn/delta/keyframe into one
   code path.

4. **Protocol hash / versioning.** The plan's Stage 4 already specifies hashing sorted component names +
   per-field `(name, type, size)` into a `uint64` exchanged at handshake. **Extend it** to include, per
   field, the *quantization parameters* (range, bit count) and the codec version, because two builds
   that agree on layout but disagree on how a `Vec3` is quantized will silently corrupt. The hash is the
   cheap insurance that makes ComponentId-as-wire-identity sound.

5. **Optional fields / arrays / strings.** FieldMeta already models `AssetPathVector`/`AssetIdVector`.
   Encode vectors as `count:varint` then elements; strings as `len:varint` + bytes. Keep these **off the
   per-tick snapshot lane** where possible — they belong to spawn/reliable state, not continuous delta.

6. **Codec is component-agnostic.** `WriteComponent(meta, ptr, baseline_ptr, BitWriter&)` and the mirror
   read. No per-component generated serializer needed beyond what reflection already provides — this is
   the same shape as the existing JSON serializer, just binary + delta + quantized. Round-trip and
   truncation-fuzz tests per reflected component, exactly as the plan says.

One design caution: **the JSON serializer keys by component *name***; the binary codec should key by
**ComponentId** (the dense int) on the wire, resolving name→id once at handshake via the protocol hash
check. Names are for disk/debug; ids are for the wire.

---

## 4. Prediction & reconciliation for a NON-deterministic physics engine

This is where the `-ffast-math`/Jolt-non-deterministic decision has the sharpest consequences, and where
the plan is correctly conservative (all of it is Stage 7, deferred). The literature strongly supports
that ordering.

### 4.1 Predict the controller, not the physics world

The universal pattern for server-authoritative games with non-deterministic or heavy physics: **predict
only the local player's own controller/avatar**, render everything else as interpolated ghosts. Source,
Overwatch, and most shooters predict the local pawn's movement + own abilities and *nothing else*
([Source networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking)). Overwatch
is the aggressive outlier — it predicts "everything (movement, all abilities, weapons) by default; teams
opt *out* per ability" ([Overwatch deep dive](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback)) —
but Overwatch can do that because its ECS isolates gameplay netcode to **3 systems out of 46**, and even
then it's a large team's multi-year effort.

For Assisi v1→v2, predict the local player controller only. Predicting arbitrary Jolt rigid bodies on the
client is a trap: without cross-machine determinism the client's Jolt and the server's Jolt diverge every
step, so you'd mispredict-correct constantly.

### 4.2 The reconciliation loop

Standard client-side prediction + reconciliation (Overwatch/Source/Fiedler):

1. Client samples input each command frame, **applies it locally immediately** (prediction), and stores
   it in a ring keyed by tick.
2. Client sends inputs to server (redundantly — see §6).
3. Server simulates authoritatively, stamps each snapshot with the **last input tick it consumed** for
   that client.
4. Client receives an authoritative snapshot for tick *T*, **snaps its predicted-entity state to the
   server's**, then **replays every buffered input from T+1 to now** to re-arrive at the present
   ([Overwatch](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback)).
   If prediction was right, the replay lands where the client already was → no visible correction.

This is where Assisi's `Registry::ReviveAt` and raw-handle EntityRef mode "earn their keep" (the plan's
words) — you rewind the *local player entity's* component state to the server value and replay. Note:
because only the controller is predicted, the "replay" is re-running the **movement system**, not the
whole Jolt world. That keeps rollback cheap and dodges the determinism problem: you're replaying a small
deterministic-enough kinematic controller, not the physics solver.

### 4.3 Misprediction smoothing

A hard snap on correction looks bad; the literature smooths it. Techniques:

- **Error accumulation + decay** (Fiedler/Overwatch): don't snap the *rendered* position to the corrected
  one; compute the error vector and blend it out over several frames. Overwatch replays to the authoritative
  present but smooths the *visual* delta.
- **Source** uses interpolation on remote entities and prediction-error smoothing on the local one.
- Keep corrections in a separate "visual offset" that decays, so simulation stays authoritative but the
  camera doesn't jerk.

### 4.4 Rocket League: the deterministic-physics counterpoint

Rocket League is the instructive contrast because it *does* predict physics objects (the ball, all cars)
— but only by making Bullet **deterministic** and replaying the whole scene almost every frame
([Cone, *It IS Rocket Science!*, GDC 2018](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf),
[Bullet in Rocket League](https://pybullet.org/wordpress/index.php/2018/03/15/rocket-league-using-bullet-physics-in-unreal-engine-4/)).
That is the road Assisi **deliberately chose not to take** (fast-math, non-deterministic Jolt). The
takeaway is not "copy Rocket League" but "Rocket League is the price of predicting shared physics: you
must buy determinism first." Since Assisi didn't, it must stay in the predict-only-the-controller camp.
Photon Quantum is the extreme version — a fully deterministic fixed-point ECS where *every* client
rollback-predicts the entire sim and no state is ever sent, only inputs ([Photon Quantum is deterministic
lockstep with rollback]) — the exact opposite architecture, listed only to mark the boundary Assisi's
non-determinism draws.

### 4.5 Server rewind lag compensation with Jolt SaveState

"Favor the shooter" lag compensation: the server keeps a **ring of recent world states** (~1 second) and,
when processing a client's command, **rewinds hitscan/collision targets to what that client saw** =
`serverTime − clientRTT − clientInterp` ([Valve Lag Compensation](https://developer.valvesoftware.com/wiki/Lag_Compensation),
[Latency Compensating Methods](https://developer.valvesoftware.com/wiki/Latency_Compensating_Methods_in_Client/Server_In-game_Protocol_Design_and_Optimization)).
The server "knows how much interpolation each client has and adjusts accordingly."

Jolt's `SaveState`/`RestoreState` is the mechanism, and the plan correctly parks this in Stage 7 (needs
`PhysicsWorld` pimpl extension + the missing `SetBodyVelocity`). Two cautions from the research:

- **Cost**: rewinding the whole Jolt world per shot is expensive; production systems rewind only *hit
  candidates'* transforms (a lightweight position history per player), not the full solver. Consider a
  cheap per-body transform ring for hit tests, reserving full `SaveState` for rarer needs.
- **Correctness bound**: Overwatch stops rewinding and switches to extrapolation above ~220 ms RTT, and
  accepts that "a victim who dodged behind cover could still die" within tolerance
  ([Overwatch](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback)).
  Lag comp is a fairness tradeoff, not free correctness — cap the rewind window.

### 4.6 Interpolation buffer (the other side of the same coin)

Remote entities render at `serverTime − interpDelay` and interpolate between bracketing snapshots
(§1.3). Interp delay should be **≥ one snapshot interval + jitter**, ~100 ms at 20 Hz, less at 60 Hz.
Assisi already has the local machinery (`PhysicsWorld::InterpolateTransforms` / `_interpolationAlpha`);
the plan reuses it for snapshot pairs, which is correct. One addition the research suggests: make interp
delay **adaptive** to measured jitter (Unity Netcode and Valve both derive it from RTT+jitter+tick-rate
rather than a constant) — start constant, make it a function later.

---

## 5. ECS-native replication design

This is the section where Assisi's existing primitives most directly shape the answer, and where the
open-source ECS-replication crates (bevy_replicon, lightyear) are the most useful mirrors.

### 5.1 Change detection: Assisi already has the right primitive (with one hole)

Assisi's model (`modules/ECS/include/Assisi/ECS/Scene.hpp`, `SparseSet.hpp`): `ACOMP(tracked)` pools get
a parallel `_changeTicks` lane; a monotonic `Scene::_changeTick` is bumped and stamped on **mutable
access** via `Add`/`GetMut`/`MarkChanged`; `Changed<T>(entity, sinceTick)` answers "written after
sinceTick." This is **structurally identical to Bevy's change detection** — Bevy stamps a per-component
`Tick` on `DerefMut` of a `Mut<T>` and exposes `Added`/`Changed` filters; `bevy_replicon` builds
replication *directly on those change ticks* to decide what to send
([bevy_replicon](https://docs.rs/bevy_replicon/latest/bevy_replicon/),
[bevy change_detection](https://ilyvion.github.io/bevy_doryen/doc/bevy_ecs/change_detection/index.html)).
It is also Unreal's **push model / dormancy** by another name: only touch what was marked changed.

So the architecture is validated: **per-connection `lastAckedTick`; each net tick, for each replicated
pool, `Changed<T>(e, lastAckedTick)` selects the delta set.** This is the dormancy optimization without
the actor graph, and it's the correct scale-appropriate choice.

**The hole — and it is the single most important finding for implementation.** `Scene::GetMut` and
`MarkChanged` stamp; but **`Query` yields mutable `Ts&` references and does NOT stamp** (Query.hpp:
`operator*` returns `std::tuple<Entity, Ts&...>`, no stamp; the only stamping paths are `Add`/`GetMut`/
`MarkChanged` in Scene.hpp). The Query header even advertises the refs as "mutable so systems can write
component data in place." **Every system that mutates a tracked component through a query therefore
produces no delta** — the replication layer would silently drop those changes. The plan flags this as the
Stage-5 blocker; the research says it is *the* correctness gate and must be closed *before* any snapshot
code, or you will debug it as a desync ghost hunt.

**Recommended stamping design (option (a) from the plan, elaborated):**

The cleanest fix that preserves the hot-path zero-cost property is a **stamping query variant** that
stamps *once per matched entity at yield time*, but only for pools that actually track changes, and only
in the mutable variant:

- Add `Scene::QueryMut<Ts...>()` returning a view whose iterator, in `operator*` (or `operator++`),
  calls `pool->Stamp(entity, ++_changeTick)` for **each required pool that `TracksChanges()`**, before
  handing back the mutable refs. Untracked pools cost nothing (the `TracksChanges()` check is the same
  gate `GetMut` already uses). Plain `Query<Ts...>()` stays read-mostly and **yields `const Ts&`** (or
  keeps yielding mutable refs but is documented as non-stamping for read-only loops).
- This is conservative (stamps even if the system doesn't actually write — same semantics `GetMut`
  already has, and the codebase already accepts "safe over-reporting, never a missed change").
- **Cost note**: stamping every yielded entity every frame bumps `_changeTick` per entity, which is fine
  for correctness but means "changed since" is *always* true for anything a mutating system touches every
  frame (e.g. a physics-driven Transform moves every tick anyway → it *should* replicate every tick). For
  components that are touched-but-rarely-changed, `QueryMut` over-reports; if that ever bites, offer a
  `MarkChangedIfDifferent` helper that compares before stamping. Don't build that yet.

The alternative (option (b), `GetMut`/`MarkChanged` discipline enforced by review + debug assert) is
cheaper but fragile: it relies on every current and future gameplay author never writing through a raw
query ref on a replicated component. Given the Query header *actively invites* in-place mutation, (b) is
a landmine. **Choose (a).** Concretely: make the mutable path explicit (`QueryMut`), make the read path
the default (`Query` → const refs), and migrate the handful of systems that write tracked components onto
`QueryMut`. A debug assert can still back it up: in debug, a tracked pool could record "last stamped
tick" and a checker could flag a tracked component whose bytes changed between frames without a stamp
(the plan's deferred "debug divergence checker").

One more subtlety worth stamping into the design: **change detection currently uses a single global
`_changeTick` per Scene, not per-component-type ticks.** That's fine for "changed since acked" as long as
`lastAckedTick` is a scene-global tick captured at snapshot-send time. Bevy uses a similar global
`change_tick` with per-component `last_changed`. The one gotcha (Bevy handles it explicitly) is **tick
wraparound** — `uint64` won't wrap in any realistic session, so Assisi can ignore what Bevy must handle.
Good.

### 5.2 Spawn/despawn & network id mapping

Local `Entity{index,generation}` handles are **not** cross-machine stable — slot reuse is local history.
The plan's `NetId` (server-allocated dense `uint32`, server↔local `Entity` map) is exactly right and is
what every ECS replication lib does (bevy_replicon maps server `Entity` ↔ client `Entity` via an
`EntityMap`; lightyear the same). Design notes from the research:

- **NetId allocation is server-authoritative and monotonic-ish**; recycle with a generation or just
  don't recycle within a session (uint32 is 4 B ids — a 32-player session won't exhaust it). If you
  recycle, put a small generation in the high bits so a stale despawn can't alias a new spawn.
- **Spawn = reliable, delta = unreliable.** Spawns/despawns and the initial full-component keyframe for a
  new entity go on the **reliable Control lane** (the plan says this); per-tick deltas go **unreliable**
  on the Snapshot lane. This split is the Tribes "events vs ghosts" lanes idea and the GNS lane model in
  Stage 1 supports it directly. Rationale: a lost spawn is unrecoverable-by-resend-of-delta (the client
  has no entity to apply the delta to), so spawn must be reliable; a lost delta self-heals next tick.
- **EntityRef fields inside components** must serialize as NetId, resolved through the map on both ends
  (the binary analogue of the JSON serializer's serial-index remap; this is why `ScopedRawEntityContext`
  must relocate down from Runtime, per the plan's module-layering note). A referenced entity not yet
  spawned on the client is the classic ordering hazard — handle it with a **deferred-resolve** (store the
  NetId, patch the `Entity` when that NetId spawns), which bevy_replicon also does.
- **Client uses `Create()`, not `ReviveAt`, for remote spawns** (plan is right) — `ReviveAt` is reserved
  for local-player rollback (§4.2) where you must restore an *exact* prior handle.

### 5.3 Per-component replication policies

Replication is not one-size. The FieldMeta/ComponentMeta layer should carry policy:

- **`Replicated{}` marker component** gates *which entities* network at all (plan, Stage 5). Good — it's
  the interest set's coarsest filter.
- **`serializable=false` transient components never hit the wire** (plan) — `Physics::RigidBody` (live
  Jolt handle), `DestroyTag`, etc. Each side rebuilds them from replicated descriptors, the
  `RebindSceneAssetsAndPhysics` pattern. This is the correct and important rule: **you replicate the
  *descriptor* (mass, shape, initial velocity), not the *live handle*.**
- **Reliable-once vs continuous per component.** Some components are set-once (a team id, a static mesh
  ref) — replicate on spawn, reliably, never again unless changed. Others are continuous (Transform,
  Velocity). The change-tick model already distinguishes these *dynamically* (a set-once component simply
  stops appearing in the delta after its tick falls behind `lastAckedTick`), so you may not need explicit
  policy flags in v1 — but a `ComponentMeta` flag like `replicationClass = {OnChange, Continuous,
  SpawnOnly}` is a cheap future hook. Don't over-engineer; change ticks cover 90% for free.

### 5.4 Interest management at 2–32 players

The blunt finding: **at 2–32 players you almost certainly need none.** The full delta of a modest world
to 32 clients is affordable. Build the `Replicated{}` marker (coarse on/off) and stop. Keep the
priority accumulator (§2.2) as the first escalation and grid/distance culling as the second — the plan's
Stage 7 "interest management" that "may share broad-phase structure with light-culling chunking." That
sharing is a genuinely good instinct: a spatial grid already built for froxel light culling is exactly
the broad-phase a distance/PVS relevance filter would want. But it is a *later* optimization; do not let
it gate v1. bevy_replicon ships with visibility control but defaults to "everything visible," and
lightyear's interest management is opt-in — same posture Assisi should take.

### 5.5 What to steal from bevy_replicon / lightyear specifically

- **bevy_replicon**: replication driven off native change ticks; `Replicated` marker component;
  server→client `EntityMap`; customizable per-type ser/de even for non-serde types
  ([bevy_replicon docs](https://docs.rs/bevy_replicon/latest/bevy_replicon/),
  [projectharmonia/bevy_replicon](https://github.com/projectharmonia/bevy_replicon)). Assisi's
  FieldMeta codec is the analogue of replicon's per-type registration — **but Assisi generates it from
  reflection**, so it doesn't need per-type user code. That's a real advantage; lean into it.
- **lightyear**: separate `Replicate` bundle, client-side prediction *with rollback* and snapshot
  interpolation as first-class opt-ins, plus interest management and even client-authoritative flows
  ([cBournhonesque/lightyear](https://github.com/cBournhonesque/lightyear)). Its architecture doc's
  key lesson: **prediction, interpolation, and interest management are orthogonal opt-in layers**, not a
  monolith. Assisi's staging (interp in Stage 5, prediction in Stage 7, interest in Stage 7) already
  respects that separation — keep it.

---

## 6. Tick rate, clocks, input redundancy, jitter buffers

### 6.1 Tick rate

Real-world tick rates ([Edgegap tick-rate explainer](https://edgegap.com/blog/game-server-tick-rate-explained-gameplay-precision-vs-infrastructure-cost),
[Gameye 64 vs 128](https://gameye.com/glossary/tick-rate/)): Apex 20 Hz, Valorant 128 Hz, CS2 64 Hz
(+subtick), Overwatch 60 Hz (128 in tournament). PvE/sandbox 20–30 Hz; competitive shooters 60–128 Hz.
The load fact that matters: **"tickrate is a server CPU cost problem, not a bandwidth problem"** — 128
vs 64 tick is only ~30 kbps/player more, but doubles simulation + hit-check + diff CPU.

Two independent rates, and the plan should keep them independent:

- **Simulation rate** = Assisi's fixed physics step (already 60 Hz; Stage 3 `simTick` increments here).
  Keep at 60.
- **Snapshot (net) rate** = a **divisor** of sim rate. The plan's open-decision #2 ("full 60 Hz or a
  divisor; start 60, make configurable") — the research says **default to a divisor: 20–30 Hz snapshots
  is the sweet spot** for a co-op/action target, halving or thirding bandwidth and diff-CPU for
  imperceptible cost given interpolation already imposes ~2 snapshots of delay. Send *input* at sim rate
  (60), send *state* at 20–30. Make it configurable, but don't default to 60 Hz snapshots — that's the
  Valorant budget for a non-competitive target.

### 6.2 Clock sync — the Overwatch "command frame buffer" model

The clean, copyable model ([Overwatch deep dive](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback),
[Overwatch GDC](https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and)):

- The client runs its clock **ahead of the server by ½·RTT + one command frame** (~96 ms at 160 ms RTT),
  so its inputs arrive *just before* the server needs them for the tick they target.
- The server maintains a small **per-client input buffer**; it wants ~1 frame of cushion so it always has
  the next input ready.
- **Time dilation / adaptive buffer**: when the server detects input starvation (buffer running dry from
  jitter/loss), it tells the client to **dilate time** — simulate a hair faster (Overwatch: ~15.2 ms
  instead of 16 ms) to rebuild the cushion; and slow down when the buffer is too deep (adding latency).
  This adaptive command-frame buffer is the single most important robustness mechanism and is what
  distinguishes a good clock from a fragile one. Unity Netcode-for-Entities does the same, deriving the
  offset from RTT and adjusting continuously ([Unity time synchronization](https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/time-synchronization.md)).

For Assisi: Stage 3 introduces `simTick`; the clock sync layer (currently implicit) should be an explicit
small module that (a) estimates server time from RTT + latest snapshot tick, (b) drives the client's
input-target tick to lead the server, and (c) adapts the lead to measured jitter. This is *not* called
out as its own stage in the plan and probably should be a named piece of Stage 3 or 5.

### 6.3 Input redundancy

Input is unreliable + **redundant**: each packet carries the **last N unacked input commands**, so a lost
packet is covered by the next (Overwatch bundles "every input since the last acked movement state" into
one packet). The plan's Stage 3 already specifies exactly this ("redundantly, last N commands per packet,
unreliable — input loss is re-covered by the next packet"). Correct; N ≈ ceil(RTT / simStep) + margin,
or just a fixed small window (e.g. last 3–8).

### 6.4 Jitter buffer

Packets don't arrive evenly. Two jitter buffers exist and both matter:

- **Server-side input jitter buffer** = the command-frame buffer above (§6.2).
- **Client-side snapshot jitter buffer** = the interpolation delay (§4.6): holding ~2–3 snapshots absorbs
  arrival jitter and one lost snapshot. Fiedler uses 4–5 frames at 60 Hz for state sync
  ([Gaffer, State Sync](https://gafferongames.com/post/state_synchronization/)); Valve's `cl_interp` is
  the same idea. Derive the depth from measured jitter + tick rate rather than a constant, eventually.

---

## 7. Implications for Assisi — opinionated verdict on N3–N7

The plan is genuinely good: the model choice (delta-snapshot + interpolation, no lockstep) is correct for
the constraints, the module layering (Net vs NetSync) is clean, the primitives (ComponentId wire identity,
FieldMeta reflection, ReviveAt, change ticks) are unusually well-suited, and the staging respects the
prediction/interpolation/interest orthogonality the literature demands. The changes below are refinements,
one genuine re-order, and precise designs for the two things the plan explicitly leaves open (the change-tick
blocker and the codec).

### KEEP (validated by the research, change nothing)

- **Delta-against-acked-baseline, per-connection `lastAckedTick`, degrade-to-resend** (Stage 5). This is
  the Quake/Source/Fiedler consensus. Correct.
- **Client interpolation at `serverTime − interpDelay`, ~2–3 snapshot buffer** (Stage 5). Correct;
  reuse of `_interpolationAlpha` machinery is exactly right.
- **NetId map; local handles never on the wire; `Create` on client spawn, `ReviveAt` reserved for
  rollback** (Stage 5). Matches bevy_replicon/lightyear. Correct.
- **Transient (`serializable=false`) components rebuilt locally from descriptors** (Stage 5). This is the
  correct "replicate the descriptor, not the live Jolt handle" rule.
- **Reliable spawn/despawn on Control lane, unreliable delta on Snapshot lane** (Stages 1/5). Correct lane
  split (Tribes events-vs-ghosts).
- **Input at sim rate, redundant last-N unreliable, server per-connection command queue** (Stage 3).
  Matches Overwatch. Correct.
- **Prediction & lag-comp deferred to Stage 7; predict only the local controller** (implied). Correct
  given non-deterministic Jolt — do NOT predict shared physics (that's the Rocket League tax you chose
  not to pay).
- **Protocol hash at handshake** (Stage 4). Correct and necessary — just extend its inputs (below).

### CHANGE

1. **The change-tick blocker — decide it now as option (a), and make it a `QueryMut` split, not a
   discipline rule.** (Stage 5 blocker / open-decision #1.) The plan leans (a); commit to it. Concretely:
   add `Scene::QueryMut<Ts...>()` whose iterator stamps each required *tracked* pool per yielded entity
   (reusing the existing `pool->TracksChanges()` gate and `++_changeTick` stamp), and make plain
   `Query<Ts...>()` the read path (ideally yielding `const Ts&`). The Query header today *invites* in-place
   mutation of raw refs, so the discipline-only option (b) is a guaranteed future desync. This must land
   **before** any snapshot code — it is the correctness foundation, not a Stage-5 sub-task. It is also
   independently useful (it's the same optimization `PropagateTransforms` wants), so it can land early and
   standalone. **Re-order: pull the stamping fix out of Stage 5 and land it as part of / right after Stage 3**,
   with unit tests proving a query-mutated tracked component reports `Changed`.

2. **Stage 4 codec: make it bit-capable from the start, not "byte-aligned now, quantize later."** The plan
   says `ByteWriter`/`ByteReader` LE, with quantization as a later pass. The research is emphatic that the
   *biggest* wins (1-bit "unchanged" field flags, smallest-three, bounded-position quantization) are
   **impossible on a byte-aligned writer**. You don't have to quantize everything in v1, but the primitive
   should be a `BitWriter`/`BitReader` so the change-bitmask and per-field quantizers can slot in without a
   format rewrite (and without re-hashing the protocol). Byte-aligned first will force a painful migration
   exactly when you need bandwidth most. Keep the round-trip + truncation-fuzz test plan.

3. **Extend the protocol hash to include quantization params + codec version** (Stage 4). Layout agreement
   isn't enough; two builds that quantize a `Vec3` differently corrupt silently. Hash per-field
   `(name, type, size, quant-range, quant-bits)` + a codec-version byte.

4. **Default snapshot rate to a divisor of sim rate (20–30 Hz), not 60** (open-decision #2). Sim stays 60;
   inputs go at 60; *state* goes at 20–30. This is the co-op/action sweet spot and halves diff-CPU +
   bandwidth for imperceptible cost given interpolation delay already exists. Keep it configurable; just
   don't default to the Valorant budget.

5. **The change-bitmask is the codec's core, and it should key on ComponentId, not name.** The existing JSON
   serializer keys by component *name*; the wire codec must key by the dense `ComponentId` (resolve name→id
   once at handshake). Per component block: `[ComponentId varint][field-changed bitmask][changed fields…]`.
   Spawn = all-ones mask (full component); delta = diff-vs-acked mask. One code path for spawn/delta/keyframe.

### ADD (missing pieces the research surfaces)

6. **An explicit clock-sync / time module** (belongs in Stage 3 or a new Stage 3.5). The plan has `simTick`
   and input buffering but never names the clock: client-time-ahead-of-server by ½RTT + one frame, server
   input-cushion target, and **adaptive time dilation** when the server's input buffer starves. This is the
   Overwatch robustness mechanism and the difference between a jittery and a solid feel. It's small but it's
   currently implicit — make it a named deliverable.

7. **Structure the server send loop as a sortable priority list from day one** (Stage 5), even if v1 always
   sends all changed entities. The priority accumulator (Tribes/Fiedler/Unreal `GetNetPriority`) is the
   *only* real answer if entity counts ever grow, and it's a superset of the plain snapshot. Building the
   send step as "collect changed entities → (optionally sort by accumulated priority) → drain to byte
   budget" costs nothing now and makes the accumulator a drop-in later with zero protocol change. Don't
   *implement* the accumulator yet — just don't hardcode a for-loop that a budget can't interpose on.

8. **Deferred-resolve for EntityRef fields** (Stage 5). A component referencing a NetId not yet spawned on
   the client must store the NetId and patch the `Entity` when that NetId arrives. Name this hazard in the
   plan; it's a classic ordering bug. (Ties to relocating `ScopedRawEntityContext` down from Runtime, which
   the plan already notes.)

9. **A per-body transform history for lag comp, separate from full Jolt `SaveState`** (Stage 7). Rewinding
   the whole Jolt world per hitscan is expensive; production systems keep a lightweight per-player position
   ring for hit tests and reserve full state save for rarer needs. Also cap the rewind window (~200–250 ms,
   Overwatch's bound) — lag comp is a fairness tradeoff, not free correctness.

### RE-ORDER (summary)

- **Move the change-tick stamping fix (`QueryMut`) out of Stage 5 to right after Stage 3.** It's the
  correctness foundation, it's independently useful (PropagateTransforms), and discovering it mid-desync is
  the worst possible time. It gates Stage 5 but should not be *inside* it.
- **Fold an explicit clock-sync module into Stage 3** (it's the natural home of `simTick` + input timing).
- Everything else keeps the plan's order. Stages 0/1/2 are correct as-is; 6 (listen server, one shared
  scene) and 7 (prediction/lag-comp/interest deferred) are correctly sequenced and correctly deferred.

### The two-sentence version

Keep the whole architecture — it's the right one for a non-deterministic-physics, 2–32-player,
performance-first engine, and the ECS primitives are unusually well-matched. The only load-bearing changes
are: **close the query-stamping hole first via a `QueryMut` split** (not a discipline rule), **make the
codec bit-level from the start** so quantization and per-field change bits aren't a later format rewrite,
and **add an explicit adaptive clock** — with a sortable priority-list send loop and a lower default
snapshot rate as cheap insurance for the future.

---

## Sources

- [Fabien Sanglard — Quake 3 Source Code Review: Network Model](https://fabiensanglard.net/quake3/network.php)
- [jfedor — Quake 3 Network Protocol](https://www.jfedor.org/quake3/)
- [bookofhook — Quake 3 Networking](http://trac.bookofhook.com/bookofhook/trac.cgi/wiki/Quake3Networking)
- [Gaffer On Games — State Synchronization](https://gafferongames.com/post/state_synchronization/)
- [Gaffer On Games — Snapshot Compression](https://gafferongames.com/post/snapshot_compression/)
- [Gaffer On Games — Reading and Writing Packets](https://gafferongames.com/post/reading_and_writing_packets/)
- [Gaffer On Games — Serialization Strategies](https://gafferongames.com/post/serialization_strategies/)
- [Gaffer On Games — Introduction to Networked Physics](https://gafferongames.com/post/introduction_to_networked_physics/)
- [Valve Developer Community — Source Multiplayer Networking](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking)
- [Valve Developer Community — Interpolation](https://developer.valvesoftware.com/wiki/Interpolation)
- [Valve Developer Community — Lag Compensation](https://developer.valvesoftware.com/wiki/Lag_Compensation)
- [Valve — Latency Compensating Methods in Client/Server In-game Protocol Design](https://developer.valvesoftware.com/wiki/Latency_Compensating_Methods_in_Client/Server_In-game_Protocol_Design_and_Optimization)
- [Edgegap — Overwatch 2016 Netcode Architecture & Rollback deep dive](https://edgegap.com/blog/game-backend-deep-dive-overwatch-2016-netcode-architecture-rollback)
- [GDC Vault — 'Overwatch' Gameplay Architecture and Netcode](https://www.gdcvault.com/play/1024001/-Overwatch-Gameplay-Architecture-and)
- [Frohnmayer & Gift — The TRIBES Engine Networking Model (PDF)](https://www.gamedevs.org/uploads/tribes-networking-model.pdf)
- [SnapNet — Netcode Architectures Part 4: Tribes](http://www.snapnet.dev/blog/netcode-architectures-part-4-tribes/)
- [Edgegap — TRIBES (1998) deep dive](https://edgegap.com/blog/game-backend-deep-dive-tribes-(1998))
- [Epic Games — Replication Graph Overview and Proper Replication Methods](https://www.unrealengine.com/en-US/tech-blog/replication-graph-overview-and-proper-replication-methods)
- [Cedric Neukirchen — Actor Relevancy and Priority](https://cedric-neukirchen.net/docs/multiplayer-compendium/actor-relevancy-and-priority/)
- [Matt Gibson — Unreal Replication: Update Frequency, Relevancy and Priority](https://www.mattgibson.dev/blog/unreal-replication-settings)
- [hzFishy — Dormancy and Relevancy](https://notes.hzfishy.fr/Unreal-Engine/Networking/Core/Dormancy-and-relevancy)
- [vorixo — The Magic of Network Managers](https://vorixo.github.io/devtricks/network-managers/)
- [StagPoint — Smallest-three quaternion compression (gist)](https://gist.github.com/StagPoint/bb7edf61c2e97ce54e3e4561627f6582)
- [Unity — QuaternionCompressor (Netcode for GameObjects)](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@1.5/api/Unity.Netcode.QuaternionCompressor.html)
- [KinematicSoup — Bit-Packing 101](https://kinematicsoup.com/news/2016/9/6/data-compression-bit-packing-101)
- [FlatBuffers documentation](https://flatbuffers.dev/)
- [Protobuf vs FlatBuffers for real-time applications](https://www.oreateai.com/blog/protobuf-vs-flatbuffers-choosing-the-right-serialization-framework-for-realtime-applications/f410bfacd86a9235f73cf2747c5026aa)
- [Jared Cone — It IS Rocket Science! The physics and networking of Rocket League (GDC 2018, PDF)](https://media.gdcvault.com/gdc2018/presentations/Cone_Jared_It_Is_Rocket.pdf)
- [Bullet Physics in Rocket League](https://pybullet.org/wordpress/index.php/2018/03/15/rocket-league-using-bullet-physics-in-unreal-engine-4/)
- [bevy_replicon — docs.rs](https://docs.rs/bevy_replicon/latest/bevy_replicon/)
- [projectharmonia/bevy_replicon — GitHub](https://github.com/projectharmonia/bevy_replicon)
- [cBournhonesque/lightyear — GitHub](https://github.com/cBournhonesque/lightyear)
- [Bevy ECS change_detection module docs](https://ilyvion.github.io/bevy_doryen/doc/bevy_ecs/change_detection/index.html)
- [Edgegap — Game Server Tick Rate Explained](https://edgegap.com/blog/game-server-tick-rate-explained-gameplay-precision-vs-infrastructure-cost)
- [Gameye — What Is Tick Rate? 64 vs 128](https://gameye.com/glossary/tick-rate/)
- [Unity — Time synchronization (Netcode for Entities)](https://github.com/needle-mirror/com.unity.netcode/blob/master/Documentation~/time-synchronization.md)
- [Unity — Interpolation and extrapolation (Netcode for Entities)](https://docs.unity3d.com/Packages/com.unity.netcode@1.9/manual/interpolation.html)
</content>
</invoke>
