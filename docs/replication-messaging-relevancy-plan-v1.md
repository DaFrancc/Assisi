# Replication messaging & relevancy plan v1 — who is told, what is said, who speaks

**Status: PLAN OF RECORD (v1.1, signed off 2026-08-03) — one adversarial
review pass and one owner design pass applied (§7).**

This plan covers the three mechanisms the replication system still lacks, as
one plan because they interlock: **relevancy** (which entities each connection
is told about at all), **control** (which connection a given entity belongs
to, decomposed so the word "ownership" never has to carry five meanings), and
**messages** (typed events in both directions — the thing other engines call
RPCs). The message design's recipient classes are computed from the relevancy
set and the control component, which is why the three ship together rather
than in sequence-of-plans.

Evidence base: docs/replication-research-rpc-survey.md (messages) and
docs/replication-research-relevancy-survey.md (relevancy and ownership), both
2026-08-03. This plan implements their §18 recommendations where it agrees
with them and says so explicitly where it departs — §7 records two places the
relevancy survey's "falls out for free" claims failed against the code. It
builds on and does not modify: transport/snapshot/baseline/body-state
machinery (plan-v4, R1–R9, built), and the five component gates
(replication-optin-plan-v1, P0–P5, built). The opt-in plan's §4 reserved the
exact seam this plan fills: *"a per-connection predicate in `SendSnapshot`,
applied before priority accumulation"* (docs/replication-optin-plan-v1.md §4).

Per the owner's direction, recorded verbatim in the relevancy survey's
framing: **base efficiency is universal to all games; information-boundary
scoping (line-of-sight, fog-of-war) stays a game-side option with engine
hooks, not an engine feature.** Every decision below that touches that line
names which side of it the mechanism lands on.

---

## 0. The problem being solved

Three absences, each currently papered over:

1. **Every connection is told about every replicated entity.** The server's
   snapshot build iterates `_liveNetIds` for every connection
   (`Replication.cpp:862`), computes despawns as `acked ∖ live`
   (`Replication.cpp:836`), and declares the world complete when the acked set
   covers the live set (`Replication.cpp:828`). There is no "to whom" axis at
   all — the opt-in plan's gates decide what an entity *says*; nothing decides
   *who is listening*. At PIE/co-op scale this costs nothing, which is why it
   was correctly deferred; the moment a game has a world larger than one
   arena, it is the scaling axis every surveyed system spends its complexity
   on (relevancy survey §12).

2. **No entity belongs to anyone.** There is no way to say "this connection's
   player is this entity." The deferred pawn work (plan-v4 §5) needs it for
   input binding; directed messages need it for addressing; disconnect
   handling needs it to know what to tear down. The relevancy survey's §14
   finding governs the shape: "ownership" in shipping engines is up to five
   unrelated jobs fused onto one pointer, and the engines that fused them
   (Unreal above all) pay in a documented bug class — every transfer is five
   simultaneous semantic changes with independent propagation delays — while
   the engines that kept the jobs apart have visibly fewer sharp edges.

3. **Nothing can happen.** The wire carries state and inputs, nothing else. A
   game cannot express "detonate", "chat line", "round started" — the event
   half of networking. Plan-v4 §5 deferred RPCs with "validation is security
   surface, not a rider"; the RPC survey's §15 is that sentence with evidence
   (four documented exploit incidents, all of them attacker-shaped messages
   meeting hand-written parsing with no central check). The deferral was
   right; the design that lands must treat the *single validated dispatch
   site* as the feature.

The gates answer "what does this entity say." This plan adds the remaining
three sentences of the model, one per mechanism:

> **Gates decide what an entity says. Relevancy decides who is listening.
> Priority decides who is heard first. Control decides who is speaking.
> Messages are for what happened, never for what is.**

---

## 1. The model

### 1.1 Relevancy: a per-connection set, filtering at one seam

Per connection `c`, a set of NetIds `R(c)`. The effective entity set for
every per-connection computation in `SendSnapshot` becomes `live ∩ R(c)`
instead of `live`. That is the entire wire-level mechanism, and the central
finding of the relevancy survey (§18.1) is that **Assisi has already built
most of the hard half**: because despawns are computed as a set difference
against the acked set and spawn is the empty-baseline path, exit and
re-entry need almost no new machinery:

- **Exit** ⇒ the entity leaves `live ∩ R(c)` ⇒ the existing despawn diff
  emits a despawn, resent until acked, and the baseline entry *and priority
  accumulator* are erased on ack (`Replication.cpp:371-375`) — a reliable
  scope-exit, strictly stronger than Quake 3's implicit absence and equal
  to everyone else's explicit close.
- **Re-entry** ⇒ the NetId is absent from `acked` ⇒ the existing
  spawn/empty-baseline path resends full state, already budget-paginated —
  Unreal's "all properties sent on re-relevancy," already implemented.
- **Client** ⇒ nothing new. A despawned mirror is a despawned mirror. The
  destroy-vs-retain fork (survey §12, the one genuine fork in the field)
  resolves to **destroy** for v1; retention is a client-side policy seam
  (§4), not wire traffic.

Two holes the review found in the survey's "falls out for free" story,
both closed by decision here (D3, D4): the body-state section is a
*fourth* loop over `_liveNetIds` with an acked-based gate, so it must
consult the effective set itself or a revoked entity keeps shipping body
state until its despawn acks; and a revoke→re-grant inside one RTT must
force the full-state path explicitly, or the client builds a corrupt
half-mirror from a delta against state it no longer has.

Membership is computed by a **relevancy provider**; the engine owns the set
and the guarantee, the provider owns the policy. Two providers ship:
`AllRelevant` (the default — the identity provider, which skips the
intersection entirely so the cost of the mechanism is zero when unused,
per the performance-first rule) and `Distance` (radius around each
connection's view anchors, with hysteresis — the industry default from
`NetCullDistanceSquared` to Fusion AoI).

**The engine guarantee, which is also the anti-cheat hook:** an entity
outside `R(c)` contributes **zero bytes** to connection `c` — no component
blocks, no body state, no messages about it. A game that wants
line-of-sight scoping writes a provider; the guarantee is what makes the
provider sufficient. Nothing further is built engine-side (owner's
direction; survey §18.3).

### 1.2 Control: one component, five jobs kept apart

The survey's §14 decomposition, applied:

| Job | Where it lives in Assisi | Why there |
|---|---|---|
| 1. State authority | **Architectural, not data.** The server writes everything; clients send `InputCommand`s and intents, nothing else (plan-v4 §3.1). | Every server-authoritative system keeps it global; making it per-entity data buys a permission lattice and a receive filter (survey §14 verdict). The opt-in plan §6 tripwire stands. |
| 2. Input binding | `ControlledBy` component | The session consumes a connection's inputs against its controlled entities; gameplay queries `(ControlledBy, …)`. |
| 3. Directed messages | `ControlledBy` component | "To the controller of E" resolves through it. No owner chain — the component is on the entity the message is about, full stop. |
| 4. Relevancy anchor | **Per-connection session state**, *defaulting* to controlled entities | Deliberately not derived from `ControlledBy` at use sites: a v1 joiner is a spectator with no controlled entity (plan-v4 §5) and still needs a viewpoint; re-fusing jobs here is how Unreal's spectate/camera leaks happened (survey §18.2). |
| 5. Prediction target | `ControlledBy`, **later** — nothing designed now | When the pawn lands, "predict what you control" is a query (Unity's `OwnerPredicted` derivation). The component is the anchor; the mechanism is future work. |

The component is deliberately named `ControlledBy`, not `Owner` — the
survey's one-line finding: *the systems with the fewest ownership bugs are
the ones whose names refuse to say "owner"* (§15). Transfer is the server
rewriting one component, replicated like any other component; the other
jobs read it through queries on arrival rather than caching a chain walk,
which is what deletes Unreal's transfer-race bug class by construction.

### 1.3 Messages: structs, two channels, one dispatch site

The RPC survey's §18, adopted whole, with two owner amendments (§7):
handler registration is generated, not hand-written; and every message
declares its direction and reliability explicitly.

- **Authoring is a struct, not a function** — `AMSG(direction, reliability)`
  on a plain struct, parsed by reflectgen's existing struct path
  (`_FIELD_RE`, `reflect_parser.py:384`, cannot parse parameter lists and
  should not learn to). A struct gets serialization, codecs, the inspector,
  and **inclusion in `NetProtocolHash()`** for free — the versioning story
  Unity built deliberately and Mirror got wrong at 16 bits.
- **Handlers are plain annotated functions, wired by codegen** (D14) — the
  registration step other engines automate (Unity's generated RPC systems,
  Unreal's UHT) is automated here too; a session's handler table is built
  at build time, with zero runtime name lookup.
- **Client→server is one channel: intents on `Lane::Control`**
  (`NetTransport.hpp:68`), extending the input path's hardened shape —
  tick-stamped, rate-limited, single receive site. One dispatch site to
  validate is the only structural answer to §15's exploit pattern.
- **Server→client is primarily a snapshot section**, appended after the
  entity blocks and body states in the same packet, so a message referring
  to a NetId established earlier in the same packet inherits its ordering
  from the framing — replicon's tick-sync guarantee, nearly free here.
  Reliable authoritative announcements ride `Lane::Control` with a tick
  stamp and client-side deferral.
- **Recipient classes, not recipient lists**: *all-relevant* (default),
  *directed*, *except-instigator*. Membership of each class is computed by
  relevancy and control — never enumerated by gameplay code, because an
  arbitrary connection list is the API through which an event leaks what
  state filtering withholds.
- **The state-first rule stands as the primary gate**: nothing that has a
  current value becomes an event (plan-v4 §5's rule, which the survey found
  every mature system warns about in prose and none enforces).

---

## 2. Decisions

Each names its alternatives and why they lost.

### D1 — `ClientId`: a session-scoped identity, assigned in the handshake

A new `NetSync::ClientId` — a **distinct wrapper type** over
`std::uint32_t`, not a typedef, so passing a transport
`Net::ConnectionId` where a `ClientId` is meant fails to compile (the two
id spaces are one honest mistake apart otherwise; review minor, promoted
to a commitment). Conventions, aligned with the codebase's uniform
zero-means-invalid rule (`InvalidNetId`, `InvalidConnection`):

- **0 = invalid / nobody.** A default-constructed id claims nothing.
- **1 = the host itself.** The listen server's own player.
- **2… = remote clients**, assigned monotonically as connections are
  added; **never reused within a session** (reuse makes "who did this"
  ambiguous in logs and late-arriving messages, for a saving of nothing).

The id is assigned when the connection is added and carried to the joiner
in `ServerHello` (which is sent at `AddConnection` — before handshake
*completion*; the id is allocated at assignment, activated on completion).
The server keeps the `ClientId ↔ ConnectionId` maps beside the NetId maps,
maintained at the same points connections are added and removed
(`Replication.cpp:223,231`).

Why it must exist: `ControlledBy` replicates, and `Net::ConnectionId` is a
transport handle — meaningful only to the server process, meaningless on
any other machine. Replicating it would replicate a pointer. Every
surveyed ECS that has the concept replicates a stable client/peer id, not
a connection handle (Unity's `NetworkId`, lightyear's peer id — survey
§15).

Consequence, stated per `NetProtocol.hpp:78-84`'s own contract ("A field
added to `ServerHello`… bump the version"): **`kNetProtocolVersion` goes
3 → 4** at M0. The version is folded into `NetProtocolHash()`
(`NetProtocol.cpp:61-66`), so builds straddling M0 refuse to pair, by
design. **Every framing change in this plan pays the same toll** — M4 and
M5 each bump again (review criticals C1/C2: the hash covers framing *only
through* the version constant, so a framing change without a bump
produces hash-equal builds that misparse each other silently).

### D2 — `ControlledBy`: server-written, replicable, never authored

```cpp
/// Which connection's player this entity is. Absent on everything
/// uncontrolled — AI, props, the world — which is the default and costs
/// nothing. Written only by the server's session layer at runtime; never
/// authored in a level (the session strips any loaded instance at host
/// time — a client id is session-scoped and would bind to whoever draws
/// that id in some future session).
ACOMP(replicable)
struct ControlledBy
{
    /// The controlling client's session id. 0 claims nothing (invalid);
    /// 1 is the host; 2+ are remote clients.
    AFIELD() std::uint32_t client = 0;

    /// What happens to this entity when that client disconnects: despawn
    /// (true — the player-spawned default) or merely lose the component
    /// (false — a world object a player was temporarily driving).
    AFIELD() bool despawnOnDisconnect = true;
};
```

- **A component, not a convention** — the survey's unambiguous answer to
  the owner's question (§15): at least three *engine-side* consumers read
  it (input binding, directed messages, disconnect cleanup), and a
  convention living inside game systems is invisible to the engine. Every
  surveyed ECS with the concept made it a component holding a client id;
  none derives it implicitly.
- **Exclusivity by construction** — one component slot, one value. No
  relationship machinery needed (survey §15). The reverse index
  (client → controlled entities) is a session-layer map, **rebuilt at
  `ReconcileNetIds` time** rather than maintained incrementally — the
  only shape that survives the editor's play/stop restore and
  undo-revive, both of which resurrect entities outside any incremental
  hook (review major 10).
- **Never persisted, enforced at the boundary.** `ControlledBy` is a
  serializable ACOMP and *would* survive a level save; a level saved
  mid-session must not bake `client = 3` into a file (review major 5,
  contradicting `NetComponents.hpp:24-26`'s own recorded principle). The
  rule: the server strips every loaded `ControlledBy` at session start —
  control is assigned at runtime by the session layer, full stop — and
  the inspector does not offer authoring it (M6). A stale instance in a
  sessionless world is inert (no consumer exists without a session).
- **Replicates to everyone**, not owner-only. Clients legitimately want
  "who controls what" (name tags, team UI), the payload is five bytes,
  and building per-connection field conditions (`COND_OwnerOnly`) for
  one component would be a new mechanism with one consumer. If a game
  ever needs to hide it, `Replicated::excluded` already can, per entity.
- **Transfer is a component write** on the server, replicated like any
  other component. The one real transfer race the survey found surviving
  decomposition — a client invoking a directed verb before learning it
  controls the entity — is a documented tolerance rule at the dispatch
  site: *input or intent for an entity you don't control is dropped and
  counted, not an error* (D10).
- **Disconnect**: for each entity in the leaving client's reverse-map
  set — despawn if `despawnOnDisconnect`, else remove the component. The
  sweep runs **before** the connection's own bookkeeping is erased
  (`RemoveConnection`, `Replication.cpp:231`), so it can still resolve
  the leaving client (review major 10). Both wire paths already exist
  (despawn rides NetId retirement; component removal rides the presence
  diff).

Hash consequence: a new replicable component enters the hashed layout —
the hash moves at M0, folded into D1's version bump.

### D3 — The relevancy set: engine-owned, provider-computed, absent by default

`Connection` (`Replication.hpp:270`) gains the per-connection set. The
representation follows the file's own conventions: a **sorted
`std::vector<NetId>`** (the acked set is already a sorted vector consumed
by `std::set_difference` / `std::includes` — `Replication.cpp:828,836`),
plus a flag for the everything case:

- `AllRelevant` (default) sets the flag and **skips every intersection** —
  the identity provider compiles down to today's exact code path. A test
  pins that filtering-disabled output is byte-identical to an
  identity-filter run of the same build (M1 DoD), which is the
  performance-first contract: zero cost when unused.
- A filtering provider maintains the sorted vector; `SendSnapshot`
  computes `effective = live ∩ R(c)` once per connection per snapshot
  (one linear merge over two sorted vectors) and every downstream
  consumer uses `effective` — **four seams, not three** (review major 1):
  the `worldComplete` test (`Replication.cpp:828`), the despawn diff
  (`:836` — which is where exit-despawns fall out), the priority loop
  (`:862`), and the **body-state loop** (`Replication.cpp:769`), whose
  existing gate is acked-based (`:791`) and would otherwise keep
  shipping body state for a revoked-but-still-acked entity every tick
  until its despawn acks.
- **Revoke→re-grant inside one round trip forces full state explicitly**
  (review major 2). The failure: the server's stale acked set says the
  client still has the entity, so re-grant sends a *delta* — but the
  client already destroyed the mirror on the despawn and would build a
  corrupt partial entity from it (`Replication.cpp:1240-1255`). The
  rule: when an entity re-enters `R(c)` while its despawn is unacked,
  the server drops its id from `connection.acked` (and its baseline
  entry), so the next snapshot takes the empty-baseline full-state path.
  The client's duplicate-despawn tolerance (`:1256`) covers the crossing
  packets. Reachable through the grant API, teleports, and boundary
  oscillation, so M1 pins it with a test.

The **provider interface** is the pluggable part (Mirror's
`InterestManagement` is the precedent shape — survey §12): given a
connection, its view anchors, and the live set, produce membership
changes. Providers run server-side at snapshot cadence (M2's Distance) or
event-driven if a future provider prefers; the engine does not care —
it consumes the set, not the method.

Alternatives rejected:
- **Per-pair virtual call** (Unreal classic's `IsNetRelevantFor`) — the
  shape Unreal built two successive systems to escape; every scaled
  system converged on set membership (survey §12, consensus fact 1).
- **Hash set instead of sorted vector** — breaks the set-algebra idiom
  the file already uses; sorted-merge beats hashing at this scale and
  keeps despawns a single `set_difference`.
- **Filtering inside the priority accumulator** — fuses two axes the
  whole field keeps separate (filters strictly before prioritizers —
  Iris's ordering, survey §0).

### D4 — The zero-bytes guarantee, pinned

An entity outside `R(c)` contributes zero bytes to `c`: no entity block
(the priority loop never sees it), no body state (D3's fourth seam — the
review proved the existing gate does *not* imply it during the exit
window), and no messages about it (the all-relevant recipient class is
computed from `effective` — D12). This guarantee is the entire
engine-side anti-cheat surface, per the owner's direction: it is what
makes a game-side strict-visibility provider *sufficient*, and it ships
for efficiency reasons anyway.

### D5 — Escape classes as policy on `Replicated`, plus grants — and one implicit rule

`Replicated` gains one field, a proper reflected enum (`AENUM` exists —
`FieldMeta.hpp:29` — and buys the hash inclusion and the inspector
dropdown for free; the draft's claim that reflectgen lacked enum support
was simply wrong, review major 7):

```cpp
/// How relevancy treats an entity carrying `Replicated`.
AENUM()
enum class Relevance : std::uint8_t
{
    /// The provider decides. The default.
    Default = 0,
    /// Every connection, regardless of provider — the correct setting
    /// for anything plot-critical, because a radius is a bandwidth
    /// tool, not a correctness tool.
    Always = 1,
    /// Only the connection named by ControlledBy ever hears of this
    /// entity.
    ControllerOnly = 2,
};

// on Replicated:
AFIELD() Relevance relevance = Relevance::Default;
```

Plus one server API for the explicit class: **per-connection grants**
(`GrantRelevance(connection, netId)` / revoke), merged into `effective`
after the provider — needed by spectator tooling and by any game-side
provider that wants to pin specific pairs.

**Plus one implicit rule (review major 6): a connection's controlled
entities are always members of its own set** — an automatic grant,
regardless of provider or anchors. Every surveyed system pins
owner-relevance; without it, D7's own use case (anchors set away from
the pawn) lets a `Default` pawn drift out of its controller's radius,
and future prediction requires the controller to always have its
subject.

This is the complete standard set — always-relevant, only-to-controller,
explicit set, off — present in every system from 1999's `SVF_*` flags to
Iris filters (survey §12, consensus fact 3; §17). **Resist inventing a
fifth.** `ControllerOnly` is the one class that reads `ControlledBy`
(its job description); the anchor deliberately does not (D7).

Hash consequence: a field added to `Replicated`'s hashed layout — the
hash moves at M2, stated in the stage.

### D6 — The Distance provider: hysteresis is mandatory, from day one

Enter radius strictly less than exit radius, plus a minimum dwell
(ticks an entity must remain outside the exit radius before the revoke
takes effect). Without both, an entity orbiting the boundary converts
into a despawn/full-respawn cycle per crossing — the one failure mode
every surveyed system either engineered around or suffered (survey
§18.1; honesty note carried over: the literature pass found *no* citable
academic treatment of boundary hysteresis — this is engine lore, with
Unreal's 5-second irrelevance grace as the shipped analogue, and the
dwell here should be far smaller since re-entry is cheap). **Dwell gates
revokes only** — the enter direction is immediate, so an anchor teleport
(level transition, spectator jump) shows the world at once rather than
after dwellTicks of emptiness (§5).

Configuration lives in `game.json`'s `networking` block
(`relevancy: { provider, radius, exitRadius, dwellTicks }`), plumbed
through `ReplicationConfig` like G2 — the session-owning layer loads
config, the server never reads the filesystem — and deliberately **not**
in the protocol hash, by G2's own argument: it changes what is *sent*,
never how bytes *decode*; the receive side is provably indifferent.

Per-pair hysteresis state (dwell counters) is provider-owned, keyed by
(connection, NetId), erased when either side dies. Cost at target scale:
tens of connections × hundreds of replicated entities × one squared
distance per snapshot tick — thousands of compares, noise. **The
RepGraph lesson is recorded, not built** (survey §18.1): if entity
counts ever make the scan bind, the fix is a shared spatial grid whose
cells hold entity lists, maintained on movement — membership evaluated
where things change, not where they are read. No grid until a measured
need; Chiara is the trigger.

### D7 — View anchors are session state, defaulting to controlled entities

Per connection, the session holds a small list of **anchor entities**
(usually one) whose Transforms the Distance provider measures from.
Explicitly set by the hosting game/session layer; **defaults to the
connection's controlled entities** when none is set.

Deliberately *not* derived from `ControlledBy` at the use site (survey
§18.2 point 3): the v1 joiner is a spectator with no controlled entity
(plan-v4 §5's "a v1 joiner is a spectator") and still needs a viewpoint;
Unreal's anchor is the *view target*, not the pawn, and spectator/camera
actors are where owner-derived anchoring leaks. (The pawn itself never
depends on anchors to stay visible to its controller — that is D5's
implicit rule.)

**A connection with no anchors under a filtering provider is
all-relevant** — fail-open. Filtering exists here for bandwidth, not
secrecy (owner's direction), so the failure mode of a missing anchor
must be "spectator sees the world," never "spectator sees nothing." A
game whose provider is an information boundary owns the opposite choice
inside that provider. The seam for client-steered anchors (a free-flying
spectator camera reporting its position) is **an unreliable intent**
once M4 lands — named here, not built.

### D8 — Dormancy is not built, and the reason is a feature

The acked-baseline core plus replicated sleep state already delivers
idle-entity silence structurally: an at-rest entity costs zero bytes
after ack, with no per-actor mode, no flush discipline, no
`FlushNetDormancy` bug class (survey §18.1 point 5 — Unreal needed
dormancy because its change detection is per-actor polling; Assisi's
per-entity acked ticks make the bandwidth win automatic). Recorded as a
decision so nobody ports the concept in later out of familiarity.

### D9 — `AMSG(direction, reliability)`: messages are reflected structs, hashed like components

A new annotation on plain structs, reusing reflectgen's existing struct
path end-to-end — parser (`_FIELD_RE` stays untouched; a message body is
`Type name;` declarations exactly like a component), codegen, JSON/binary
codecs, field metas, goldens. A `MessageMeta` registry mirrors
`ComponentRegistry`: sorted by name, dense ids assigned at finalize,
duplicate names a hard error.

**The grammar: two mandatory positional arguments, direction then
reliability, further arguments after** (owner decision, §7):

```cpp
AMSG(intent, reliable)      struct PlantBomb    { ... };  // must arrive
AMSG(intent, unreliable)    struct PingMarker   { ... };  // freshest wins
AMSG(event,  unreliable)    struct Detonated    { ... };  // rides the snapshot
AMSG(event,  reliable)      struct MatchStarted { ... };  // held-until-ordered
AMSG(event,  unreliable, independent) struct ChatLine { ... };  // names no entity
```

A message missing either argument, or with them swapped, is a reflectgen
hard error naming the rule — the `replicated`→`replicable` tombstone
style, so the error teaches the grammar. Mandatory-explicit means the
declaration states the full wire contract with no defaults to memorize,
and a changed default can never silently reclassify existing messages.
Both arguments are consumed by the machinery: direction tells the
handler generator which side binds (D14) and gives the dispatch site a
free check (an `event` type arriving *from* a client is rejected before
anything else — the vocabulary itself says clients don't speak it);
reliability picks the channel (D10/D11). Reliability is per *type*,
never per send: a message that is sometimes reliable has an unclear
meaning, and per-type declaration is what lets the panel show reliable
traffic per type.

Further mechanics:

- **Dense message ids go on the wire** (varint). Safe by the same
  argument the component codec already relies on: the handshake refuses
  on `NetProtocolHash()` mismatch, and hash-equal builds have identical
  message sets, hence identical dense ids.
- **Every message layout folds into `NetProtocolHash()`** — add a
  message type, the hash moves, mismatched builds refuse. The
  versioning story Unity built deliberately (`RpcCollectionVersion`),
  Godot approximates with an undiagnosable whole-set checksum, and
  Mirror gets wrong at 16 bits. Assisi gets it from the fold.
- **Payloads are length-prefixed** on the wire, so a reader can skip a
  message body it cannot decode. Stated honestly (review major 8): with
  dense ids this buys *skip*, not *tolerate* — a peer with a different
  message set would misdispatch colliding dense ids, so true
  forward/backward tolerance requires stable (name-derived) ids. That
  migration — stable ids plus a handshake policy switch — is the
  recorded seam for the day Assisi ships a title with independent
  client/server patch cadences; it is deliberately not built now, and
  the length prefix is what keeps it a policy change rather than a
  format rewrite.
- **Fields declared per message**: a message about an entity carries
  `NetId` as a declared field (addressing is data, not a receiver —
  there is no "call this on that object" because there is no object).
  reflectgen validation: `AMSG` rejects `AFIELD(norep)` (meaningless —
  a message *is* its wire form) and rejects `transient`.

Alternatives rejected:
- **Function-based RPCs** (`Server_DoThing(int32_t x)`) — requires a
  genuinely new signature parser, a `FunctionInfo` model, and dispatch
  codegen, for the sole benefit of nicer spelling; loses free hash
  inclusion. The survey's load-bearing recommendation, four independent
  precedents (`IRpcCommand`, `NetEvent`, `DeterministicCommand`,
  replicon).
- **A message base class / virtual dispatch** — nothing else in the
  reflection system works that way; structs + registry is the house
  idiom.

### D10 — Client→server: one intent channel, one dispatch site, reject-don't-clamp

A client sends **intents**: `{ messageId, clientTick, payload }` on
`Lane::Control` — reliable or fire-and-forget per the type's declared
reliability (`AMSG(intent, reliable)` sends reliable; `unreliable` sends
best-effort on the same lane; both are equally *untrusted*, because
reliability is about delivery, not trust). The natural unreliable-intent
cases are the spammy, freshest-wins asks — map pings, "look here"
markers — where a resent stale message is worse than a lost one.

The server's message loop grows one `MessageType::Intent` case beside
`HandleInput` (`Replication.cpp:391`), which lands every intent in **one
dispatch site**, in this order (the input path's own flood-costs-a-
comparison ordering — review minor):

1. **Envelope read** — message id + tick, fixed-size, bounded.
2. **Direction check** — an `event`-declared type arriving as an intent
   is dropped and counted (D9's free check).
3. **Rate limit per connection per message type** — a sliding window
   beside the input path's existing one (`Replication.hpp:307-308`),
   *before any payload work*, so a flood costs a comparison. Unreal
   retrofitted `FRPCDoSDetection`; the shape is known in advance here.
4. **Tick staleness window** (review major 9): `clientTick` outside
   `[serverTick − staleWindow, serverTick + leadWindow]` is dropped and
   counted — the `InputCommandQueue` stale-drop's analogue, and
   load-bearing for unreliable intents, where out-of-order arrival is
   normal and a stale ping must not time-travel.
5. **Payload decode**, hostile-input caps like every other reader.
6. **Field-range validation — reject and log, never clamp.** The input
   path clamps (`ClampInputCommand`, `InputCommand.hpp:75-91`) because a
   stick can legitimately saturate. An intent with an out-of-range field
   means the client is lying or the builds disagree — clamping converts
   a detectable attack into a silently-accepted one. Ranges come from
   the same `AFIELD(min/max)` metadata the inspector reads, free.
7. **Control tolerance rule** (with D2): an intent naming an entity the
   sender does not control is dropped and counted, not an error —
   transfer propagation delay makes this reachable by honest clients.
8. **Dispatch** through the generated handler table (D14). No handler
   registered ⇒ dropped and counted.

**The host's own intents enter at step 2** (review major 3): a listen
server's player is not a connection, so host-originated intents are
submitted locally with sender = ClientId 1 and flow through the same
checks (minus transport-level framing) and the same handlers. One door
means *one*, including for the person hosting.

Per-type received/rejected/rate-limited counters live in
`ConnectionDiagnostics` and the Network panel. No per-message
`_Validate` hook — Unreal's shape, optional there; the single site does
the job better. No return values, no request/response — a reply is
state, and Assisi has a state channel.

### D11 — Server→client: a snapshot section for events, `Control` for announcements

Two forms, chosen by the type's declared reliability (D9), deliberately
only two (the survey's warning: both Quake 3 and Unreal hit the same
reliable-buffer cliff — Unreal's `RELIABLE_BUFFER` is 256 and overflow
*closes the connection* — and both mitigations are asking every call
site to budget against a global; do not build general reliable-ordered
messaging):

**a) `AMSG(event, unreliable)` — the snapshot section (the default
form).** Appended to the snapshot packet after the entity blocks and the
body-state section. A message referring to a NetId established earlier
*in the same packet* inherits replicon's ordering guarantee from the
framing itself — the survey's "one genuine improvement" (RPC survey
§10), nearly free here because the snapshot already carries
`serverTick`/`baselineTick` (`NetProtocol.hpp:162-184`) and the wire
order already puts entity blocks first (plan-v4 §3.3).

- **Held, not dropped**: a message about an entity the connection does
  not yet know (budget-cut spawn, mid-join pagination) goes into a
  small per-connection pending queue keyed by NetId, drained into the
  section the moment the NetId is acked or written in the same packet —
  the body-state gate's rule with the opposite resolution, because a
  state can wait for the next tick and an event cannot be regenerated.
  **Eviction is specified, not discovered**: a held message whose
  target NetId despawns is dropped and counted; a hard cap per
  connection (config) drops oldest-first and counts. Held entries are
  indifferent to `ResetBaselines` (`Replication.cpp:381` — it does not
  touch `acked`, so held-queue semantics survive it unchanged; review
  minor, decided). Both counters surface in diagnostics — a growing
  held queue must be visible.
- **`independent`** (third-position argument): names no entity, skips
  the hold entirely (chat lines, round banners, hit-sparks referencing
  nothing).
- Loss is accepted once sent — this form is for events whose loss is
  tolerable. The section shares the snapshot byte budget with a small
  reserved floor so a full entity budget cannot permanently starve the
  section (exact split an implementation constant, stated in M5).

**b) `AMSG(event, reliable)` — Control-lane announcements
(`MessageType::Announcement`), rare.** For authoritative events that
must arrive (match start, level transition prep). The envelope carries
`serverTick`; the client **defers dispatch** until its applied snapshot
tick reaches the stamp and every NetId field resolves — replicon's
client-side hold, needed only on this path because the snapshot path
gets ordering from framing. `independent` announcements dispatch on
arrival. The panel shows announcement rates, and the documented
contract is that a type sending at snapshot cadence belongs in (a) or
is state wearing an event costume.

**The host receives its own events locally** (review major 3): messages
whose recipient class includes the host (which is all of them for
all-relevant — the authority sees everything) are enqueued into a local
delivery queue and dispatched through the same client-side handler path
at the end of the server's tick, after all of that tick's state
mutations — the same "world is at least as new as the message" property
a remote client gets from packet ordering. Without this, chat — this
plan's own example — never reaches the host's screen.

### D12 — Recipient classes: computed, never enumerated

Three classes, resolved server-side per message when the section is
built (survey §18, revised bullet — adopted with its reasoning):

- **All-relevant** (default): the message about entity E goes to every
  connection whose `effective` set contains E — structural, because the
  section is built per connection alongside the entity blocks — plus
  the host's local queue. `independent` messages go to every ready
  connection and the host.
- **Directed**: one recipient — the controller of a named entity
  (resolved via `ControlledBy` → ClientId → connection, or the local
  queue when the controller is the host), or an explicit `ClientId`
  for session-level messages.
- **Except-instigator**: all-relevant minus one `ClientId` — for events
  the instigator already predicted or displayed locally (the
  `COND_SkipOwner` / `NotMe` pattern every system has). The host can be
  the excluded instigator like anyone else.

**No arbitrary per-call recipient lists.** A gameplay-computed
connection list bypasses the relevancy boundary — it is the API through
which an event leaks what state filtering withholds — and it duplicates
in every call site the recipient computation relevancy owns in one
place. A game needing an exotic scope expresses it as explicit grants
(D5) plus the standard classes.

### D13 — The state-first rule gets teeth, gently

Two cheap reinforcements, both visibility rather than enforcement (no
surveyed system enforces the rule, and FishNet's `BufferLast` shows
where enforcement pressure leads):

- The authoring cost of a message (declare a struct, write a handler
  function) stays deliberately **higher** than marking a field
  replicable (one word). Auto-registration (D14) removes the wiring
  step, not the authoring step — the gap survives.
- The Network panel shows **per-type message rates**. "We are streaming
  state through events" becomes a number someone sees, not an
  archaeology finding.

### D14 — Handlers: annotated functions, wired by codegen, zero ambiguity

Owner decision (§7): handler registration is generated, not
hand-written — the registration step is fully mechanical, and the
engines that ship declarative messaging automate it (Unity's generated
RPC systems from `IRpcCommand` structs, Unreal's UHT registration).

**Authoring:**

```cpp
// In any reflected header — the declaration IS the registration:
AMSG_HANDLER() void HandleChatSend(NetContext &ctx, const ChatSend &msg);

// In a .cpp — just the body:
void HandleChatSend(NetContext &ctx, const ChatSend &msg)
{
    ctx.session.Send(ChatLine{ .from = ctx.sender, .text = msg.text });
}
```

- **One fixed signature**: `void Name(NetContext &, const T &)`. Return
  values, extra parameters, by-value messages, or a missing context are
  reflectgen hard errors naming the expected shape. The rigidity is
  what keeps the scan a fixed-shape pattern match instead of a C++
  signature parser — extracting one type from one rigid pattern does
  not reopen the function-parsing problem D9 rejects, because the wire
  form still comes entirely from the struct.
- **Functions, not lambdas** — forced and preferred: reflectgen scans
  declarations in headers; a lambda has no declaration, no name, no
  linkage. Handlers marked `static` or in anonymous namespaces are
  rejected (internal linkage — the generated table could not reference
  them).
- **`NetContext`** carries what a handler needs: `sender` (ClientId —
  meaningful for intents), the session, and the scene. Handlers are
  ordinary gameplay code — query, mutate, spawn, send further messages;
  on the server their mutations *are* the replication (the state
  channel picks them up); on clients they do the local/cosmetic work.
  Functions-over-context is the same shape ECS systems already use, and
  it is why no captures are needed.
- **Dispatch timing is defined, not incidental**: intents dispatch on
  the server at a fixed point before the simulation step consumes their
  consequences; events dispatch on the client after the packet's state
  is applied (and on the host at end-of-tick, D11). Handlers never run
  from a network thread and never race systems.
- **Zero-ambiguity binding**, three layers (owner requirement):
  1. The generated table names every handler **fully qualified and
     anchored at global scope** (`::MyGame::Chat::HandleChatSend`) —
     reflectgen already tracks enclosing namespaces for component
     registration; no `using` directive, ADL, or nearer-scope name in
     the generated TU can redirect the lookup.
  2. The address is taken with an **explicit signature cast**
     (`static_cast<void (*)(NetContext &, const ChatSend &)>(…)`), so
     overload sets resolve to exactly the declared shape, at compile
     time of the generated TU.
  3. **One handler per message type per side**, enforced by the same
     cross-module aggregation pass that counts replicable types for the
     mask width: two handlers for one type is a build error naming both
     declaration sites. Not first-wins, not link-order.
- **Why codegen and not static-init registrars**: modules are static
  libraries, and linkers dead-strip registrar objects nobody
  references — handlers would silently vanish by link order. The
  generated table references every handler by name, forcing linkage.
  This is the same class of argument that chose build-time aggregation
  for the mask width (opt-in plan D5).
- The manual `OnIntent<T>` / `OnEvent<T>` API remains underneath as the
  substrate the generated table calls into — and as the test surface.
  Game code writes annotated functions.

Direction comes from the message type's own declaration (D9), so the
generator knows an intent handler binds server-side and an event
handler client-side (the host binds both — it is authority and player
at once).

---

## 3. Stages

Each stage is independently landable and leaves every existing test
green. DoDs distinguish terminal-verifiable from eyes-needed, per the v4
convention. **Hash and version movements are stated per stage**; a
framing change always bumps `kNetProtocolVersion` (D1), and builds
straddling any moving stage refuse to pair, by design.

### M0 — ClientId and ControlledBy

- `ClientId` wrapper type (0 invalid / 1 host / 2+ remote, monotonic,
  never reused); assignment at connection add; `ServerHello.clientId`;
  **`kNetProtocolVersion` 3 → 4**; the `ClientId ↔ ConnectionId` session
  maps.
- `ControlledBy` per D2; reverse map rebuilt at `ReconcileNetIds`;
  disconnect sweep ordered before connection-bookkeeping erasure;
  session-start strip of loaded instances.
- **Tests (terminal):** handshake round-trip carries the id; ids
  monotonic, never reused across a disconnect/reconnect; `ControlledBy`
  replicates and appears on the mirror; disconnect despawns a
  `despawnOnDisconnect=true` entity end-to-end and strips the component
  from a `=false` entity; transfer propagates as an ordinary delta; a
  level file carrying `ControlledBy` loses it at host time (strip
  pinned); a client-fabricated `ControlledBy` write changes nothing
  server-side; reverse map correct across editor play/stop restore
  (revived entities re-resolve).
- **DoD (terminal):** all green; hash moved once (version bump + new
  replicable component), stated in the commit.

### M1 — Relevancy core: the set, the seams, the lifecycle

- `Connection` gains the sorted `relevant` vector + all-relevant flag;
  provider interface; `AllRelevant` provider (identity — flag set, no
  intersection executed).
- `SendSnapshot` computes `effective` once and uses it at **all four**
  seams: `worldComplete` (`:828`), despawn diff (`:836`), priority loop
  (`:862`), body-state loop (`:769`).
- The re-grant-during-inflight-despawn acked-drop rule (D3).
- Explicit-grant API and the implicit controlled-entities grant (D5) —
  M1's tests need grants to exercise filtering without waiting for M2's
  provider.
- **Tests (terminal):**
  - filtering disabled ⇒ wire output byte-identical to an
    identity-filter run (the zero-cost pin);
  - revoking a grant ⇒ despawn emitted, resent until acked, baseline
    and priority entries erased on ack;
  - re-granting after ack ⇒ full state via the empty-baseline path,
    correctly budget-paginated;
  - **re-granting before the despawn acks ⇒ full state, not a delta**
    (the acked-drop rule, pinned — fails without D3's rule);
  - **zero-bytes pin (D4)**: an entity outside `R(c)` produces no
    entity block **and no body-state bytes** for `c` — including
    during the exit window, while other connections still receive
    both (fails without the fourth seam);
  - a connection's controlled entity survives a revoke attempt (the
    implicit grant, pinned);
  - `worldComplete` is per-connection truth: a filtered connection
    reports complete while a fresh unfiltered one does not;
  - keyframe sweep interacts correctly with a filtered set;
  - priority accumulators for out-of-set entities do not climb.
- **DoD (terminal):** all green; hash and version unmoved (no layout or
  framing change — pinned by the byte-identical test).

### M2 — Distance provider, anchors, escape classes, diagnostics

- View anchors per D7 (session state, default = controlled entities,
  fail-open when empty).
- `Distance` provider per D6: enter/exit radii, dwell (revokes only),
  per-pair state, config via `ReplicationConfig` from `game.json` (not
  hashed).
- `Replicated::relevance` per D5 (`AENUM`), consulted when merging
  provider output into `R(c)`.
- `ConnectionDiagnostics` gains relevant-set size and enters/exits per
  second; Network panel rows for both — **thrash must be visible, not
  inferred**.
- **Tests (terminal):** an entity crossing the enter radius appears
  immediately; one hovering between radii does not thrash (dwell pinned
  with a scripted oscillation); an anchor teleport re-fills the set
  without dwell delay; `Always` survives a filtering provider;
  `ControllerOnly` reaches exactly the controlling connection (and
  nobody when uncontrolled); anchorless connection under Distance
  receives everything (fail-open pinned); config absent ⇒ AllRelevant
  (default pinned); malformed relevancy block warns and changes nothing
  (the quantization loader's contract).
- **DoD (terminal + eyes):** terminal all green; hash moves once
  (`Replicated` field + `Relevance` enum enter the hashed layout),
  version unchanged (no framing change). Eyes: the panel rows during a
  two-editor LAN session with a small radius — entities popping in/out
  at the boundary look sane, counters move as expected.

### M3 — `AMSG` grammar, codecs, registry, hash

- reflectgen: `AMSG(direction, reliability[, extras])` with both
  positional arguments mandatory and ordered (missing/swapped ⇒ hard
  error naming the rule); `independent` as an extra; validations per D9
  (`norep`/`transient` rejected); `MessageMeta` + registry (sorted,
  dense ids, duplicate-name hard error); `AMSG_HANDLER` declaration
  scanning per D14 (fixed signature enforced; static/anonymous
  rejected); goldens regenerated (own commit).
- JSON + binary codecs via the existing field paths; length-prefixed
  payloads; hostile-input caps like every other reader.
- `NetProtocolHash()` folds every message layout, direction, and
  reliability (a reclassified message must refuse to pair — its wire
  meaning changed).
- Generated handler-table TU per D14: fully-qualified `::`-anchored
  references, signature casts, cross-module one-handler-per-type
  aggregation check.
- **Tests (terminal):** round-trip (JSON + binary) on a registered test
  message; truncation/fuzz on the binary read; grammar rejections
  (missing arg, swapped order, `norep`, lambda-shaped/static handler);
  adding a registered test message type moves the hash, removing it
  restores it (property test against a build that registers at least
  one AMSG — the guard the review's minor asked for); flipping a test
  type's reliability moves the hash; dense-id stability under
  hash-equality; skip-on-length for an unknown id; duplicate-handler
  aggregation failure (build-level test); overload disambiguation
  (two same-named handlers, different namespaces and different message
  types, both bind correctly — compile-level).
- **DoD (terminal):** all green; hash moves (message layouts enter it)
  — and will move again with every future `AMSG`, which is the design.
  Version unchanged (M3 defines types; no packet framing changes until
  M4).

### M4 — The intent channel

- `MessageType::Intent` framing; **`kNetProtocolVersion` 4 → 5**
  (review critical C2 — a new `MessageType` is a framing change and
  the hash covers framing only through the version constant); envelope
  `{ messageId, clientTick, payload }`; reliable or best-effort per the
  type's declaration, both on `Lane::Control`.
- The single dispatch site per D10, in D10's order (envelope →
  direction → rate → staleness → decode → range → control → dispatch).
- Host-local intent submission entering at the direction check with
  sender = ClientId 1.
- Generated table consulted for dispatch; `NetContext` per D14.
- `ConnectionDiagnostics`: per-type received/rejected/rate-limited/
  stale-dropped counters; panel rows.
- **Tests (terminal):** a registered intent arrives at its handler with
  the right sender `ClientId`; the host's own intent flows through the
  same site and handler; out-of-range field ⇒ rejected, counted, **not
  clamped** (contrast test against the input path's clamp); an
  `event`-declared type sent as an intent ⇒ dropped at the direction
  check; stale and future-dated intents dropped by the tick window
  (out-of-order unreliable intent pinned); rate flood ⇒ limited per
  type before payload decode, without disturbing other types or the
  input path; unknown id / malformed / oversized ⇒ dropped, counted,
  connection lives; unhandled type ⇒ dropped, counted.
- **DoD (terminal):** all green; version bumped, stated in the commit.

### M5 — Server→client: the snapshot section and announcements

- Snapshot message section after body states; **`kNetProtocolVersion`
  5 → 6** (review critical C1 — a new snapshot section is exactly the
  framing change `NetProtocol.hpp:78-84` names); recipient classes per
  D12; reserved byte floor within the snapshot budget.
- `MessageType::Announcement` framing for `AMSG(event, reliable)`
  (named, per the review) — same version bump covers it.
- Held queue per D11a: keyed by NetId, drained on ack-or-same-packet,
  evicted on target despawn or cap, both counted; `independent`
  bypass.
- Client-side deferral for announcements (applied-tick + NetId
  resolution); host-local event queue dispatched at end of server
  tick.
- Client dispatch through the generated table, **after** the packet's
  entity blocks and body states.
- **Tests (terminal):** a message about an entity spawned in the same
  packet dispatches after the spawn is applied (the ordering
  guarantee, pinned); a message about a budget-cut entity is held,
  then delivered exactly once when the spawn lands; held eviction on
  despawn and on cap, counters correct; except-instigator excludes
  exactly the instigator; directed reaches exactly the controller and
  is dropped-with-count when the entity is uncontrolled; **the host
  receives all-relevant and directed-to-host events through the local
  queue** (chat-reaches-host, pinned); a filtered connection receives
  no message about an out-of-set entity (D4's message half, pinned);
  announcement deferral: an announcement stamped tick T naming entity
  E dispatches only once E exists and applied tick ≥ T; section
  starvation both directions: a full entity budget leaves the
  reserved message floor, and a join-flood's held backlog does not
  starve entity state (§5).
- **DoD (terminal):** all green; version bumped, stated in the commit.

### M6 — Editor, panel, documentation

- Network panel: relevant-set size, enter/exit rates (M2), per-type
  message rates, held/evicted/rejected/stale counters (M4/M5) — the
  visibility half of D13.
- Inspector: `Replicated.relevance` as the AENUM dropdown on authoring
  entities, with P4's honesty rules extended to it — **a mirror renders
  neither `relevance` nor `ControlledBy` as authorable** (a mirror's
  `Replicated` is default-constructed fabrication, and control is
  server-written); mirrors show observed facts only. Undo/redo covers
  the new inspector rows through the same undo-capable path P4
  verified.
- Docs: this plan flips to **plan of record**; plan-v4 §5's RPC and
  interest-management deferrals gain pointers here; the model sentence
  (§0) lands in the module README; both survey docs gain the "what
  Assisi chose and why" postscript the opt-in survey got.
- **DoD (eyes):** the panel during a two-editor session with Distance
  filtering and a chatty test message — rates legible, counters move,
  nothing renders fabricated data; undo of a `relevance` edit restores
  it. Terminal: editor-state tests extend to the new inspector rows.

---

## 4. Non-goals, with named seams

Declared so omission is a decision, and each has a landing place when
its trigger arrives.

- **PVS / occlusion-based transmit culling.** BSP-era machinery; no
  visibility structure exists here and none should be built for
  networking (survey §17). Seam: a game-side provider.
- **A replication graph / spatial grid.** Fortnite's numbers are three
  orders of magnitude past target scale. Seam: D6's recorded grid
  design, triggered by Chiara showing the Distance scan binding.
- **Dormancy.** D8 — the acked baseline already is the better version.
- **Per-entity / transferable state authority.** The opt-in plan §6
  tripwire stands verbatim: the day it lands, G2 grows a server-side
  receive filter and intents grow authority checks. Until then,
  authority is architecture.
- **Engine-side line-of-sight / fog-of-war.** Owner's direction. The
  engine's whole obligation is D4's zero-bytes guarantee plus the
  provider/anchor/grant hooks — all shipped for efficiency reasons.
- **Client-side retention of out-of-scope entities** (Source's
  keep-and-dormant). Seam: a client policy layered on the same wire
  traffic (keep the entity, strip liveness); build nothing.
- **Client-reported view anchors** (free spectator cameras steering
  Distance). Seam: an `AMSG(intent, unreliable)` type, once M4 exists;
  the session-layer anchor API is where it plugs in.
- **Per-world relevancy/message policy.** The engine has multi-world
  support and `NetSession` binds one scene at construction; when a game
  needs per-world overrides they live in the world profile as a
  constructor-time filter — same shape as G2, different owner (the
  seam the opt-in plan named for its gates, named here for these axes
  too; review major 10).
- **Prediction.** `ControlledBy` is the anchor (job 5); the mechanism
  belongs to the pawn/prediction work plan-v4 §5 deferred. Nothing
  here designs it; nothing here obstructs it.
- **General reliable-ordered messaging / message priorities.** The
  Q3/Unreal buffer cliff (RPC survey §18 point 5). Two channels are
  the design, not the v1 subset of it.
- **Request/response, return values, buffered/last-value messages.**
  A reply is state; `BufferLast` is the state-first rule's failure
  mode with a feature name (RPC survey §18).
- **Stable (name-derived) message ids / tolerate-mode handshake.** The
  independent-patch-cadence deployment model's requirement, recorded in
  D9 as a policy migration the length-prefixed wire form deliberately
  keeps cheap. Not built until that deployment model exists.
- **A game-scope message veto** (G2's shape for message types). No
  incident motivates it; it composes as a registry-time filter if one
  ever does.

---

## 5. Risks and open implementation details

- **Message-section budget interaction.** The reserved floor (D11a)
  must not starve entity state during a join flood, nor vice versa.
  The join case is the stress point: a joining connection has maximal
  entity backlog and (if the game is chatty) maximal held messages.
  M5 tests both directions; the exact floor constant is a keyboard
  decision recorded in the commit.
- **Dwell direction.** D6 pins it — dwell gates revokes only — but the
  implementation must resist the symmetric-hysteresis reflex; M2's
  teleport test is the tripwire.
- **`Lane::Control` load.** Handshake, despawn-critical traffic, and
  now intents and announcements share the lane. GNS handles per-lane
  ordering; the risk is human — someone marking a chatty type
  `reliable`. The per-type rate visibility (D13) is the mitigation; if
  a real incident occurs, a dedicated lane is a constructor-time
  config change (`LaneCount`, `NetTransport.hpp:76`).
- **Host-local delivery ordering.** D11's end-of-tick dispatch for the
  host must not observe a world mid-mutation; the dispatch point is
  after all state writes for the tick, before the next tick's input
  consumption — M5 should assert the invariant in debug.
- **Golden churn.** M3 regenerates reflectgen goldens; own commit, per
  the opt-in plan's convention, so the diff reviews against one cause.
- **Aggregation-pass coverage.** D14's one-handler-per-type check and
  the handler scan share the opt-in plan's build-time aggregation
  machinery and inherit its known boundary: out-of-tree game modules
  must route reflected headers through `assisi_reflect()` to be
  scanned. Same trigger, same revisit condition (dynamically-loaded
  modules).

---

## 6. What this plan does not solve, honestly

Relevancy decides *who is told*; it does not make the world larger than
one server's simulation (sharding, seamless worlds — different
project). `ControlledBy` decides *whose entity this is*; it does not
give clients authority over it (architectural, D2), and it does not
implement possession UX, input consumption, or prediction — it is the
component those will read. Messages carry *what happened*; they do not
carry state, and the plan deliberately keeps them worse at carrying
state than the state channel is (D13). Anti-cheat beyond the zero-bytes
guarantee — server-side sanity simulation, statistical detection, trust
scoring — is a game concern the engine only promises not to undermine.

---

## 7. Review record

**First pass (2026-08-03, fable agent, adversarial, full code
verification).** Every cited line opened; one off-by-one found
(`LaneCount` — corrected). Verdicts: 10 of 13 decisions agreed outright
or with amendments; two criticals and ten majors, all accepted:

- **C1/C2 — the hash-versioning story was broken at M4 and M5.** The
  draft claimed the hash "covers framing"; it covers framing only
  *through* `kNetProtocolVersion` (`NetProtocol.cpp:61-66`), so the new
  `Intent` message type and the new snapshot section were framing
  changes that hash-equal builds would misparse silently — the exact
  failure the handshake exists to prevent. Fixed: every framing stage
  bumps the version (M4 → 5, M5 → 6), and D1 states the rule.
- **Zero-bytes had a fourth loop.** `WriteBodyStates` iterates
  `_liveNetIds` independently (`Replication.cpp:769`) with an
  acked-based gate (`:791`); the relevancy survey's §18.1 claim that
  the existing gate "already implies" filtering is wrong against the
  code — a revoked entity kept shipping body state until its despawn
  acked. Fixed: D3's four seams; M1 pins the exit window.
- **Revoke→re-grant inside one RTT built a corrupt half-mirror** via
  the unknown-NetId delta path (`:1240-1255`). Fixed: the acked-drop
  rule in D3, pinned in M1.
- **The listen-server host was nobody**: no loopback connection exists
  by design (`NetSession.hpp:12-25`), so all-relevant events never
  reached the host's own UI and host intents bypassed the dispatch
  site. Fixed: host-local submission (D10) and the host-local event
  queue (D11); chat-reaches-host pinned in M5.
- **`ClientId 0 = host` inverted the codebase's zero-means-invalid
  convention** and armed default-constructed `ControlledBy{}`. Fixed:
  0 invalid / 1 host / 2+ remote (D1).
- **`ControlledBy` persisted session-scoped ids into level files.**
  Fixed: never-authored rule, session-start strip, no inspector
  authoring (D2), pinned in M0.
- **No owner-relevance rule** — a controller's own pawn could leave its
  radius. Fixed: the implicit grant (D5).
- **The draft's D5 falsely claimed reflectgen lacked enum support**;
  `AENUM`/`FieldType::Enum` exist (`FieldMeta.hpp:29`). Fixed:
  `Relevance` is a proper reflected enum.
- **"Tolerate-capable" overclaimed**: a length prefix over dense ids
  buys skip, not tolerance — misdispatch on colliding ids. Fixed: D9
  states it honestly; stable ids recorded as the migration seam (§4).
- **Intents lacked a tick-staleness rule**; fixed (D10 step 4),
  load-bearing for unreliable intents.
- **Reverse-map lifecycle and sweep ordering unspecified**; fixed
  (rebuild at `ReconcileNetIds`, sweep before erasure — D2); the
  per-world seam added to §4.
- Minors, all applied: `LaneCount` line ref; ServerHello-timing wording
  in D1; rate-limit-before-decode ordering (D10); the M1 byte-identical
  test rephrased as identity-filter comparison; M3's hash-property test
  guarded by a registered type; the held-queue/`ResetBaselines`
  question decided (indifferent — it does not touch `acked`);
  mirror-side honesty extended to `relevance`; M6 undo coverage;
  `MessageType::Announcement` named; the ClientId wrapper type promoted
  from hedge to commitment.

The pass also positively verified the load-bearing lifecycle claims:
exit-despawn resent-until-acked with baseline *and* priority erasure on
ack (`:371-375`); re-entry riding the empty-baseline path; worldComplete
per-connection soundness including `ControllerOnly`; the keyframe-sweep
interaction; the M0/M2 hash mechanics; dense-id safety by the component
codec's precedent; `_FIELD_RE`'s inability to parse functions; and both
surveys' §18 adopted faithfully apart from the tolerance substitution.

**Second pass (2026-08-03, owner design review).** Four decisions,
folded in as D9/D14 and the M3/M4 stage updates:

1. **Handler registration is generated, not hand-written** — the
   reflection system does the wiring (`AMSG_HANDLER` declarations,
   generated binding table). Codegen chosen over static-init
   registrars because static libraries dead-strip unreferenced
   registrar objects.
2. **`AMSG` takes two mandatory positional arguments** — direction then
   reliability, in that order, extras after; missing or swapped is a
   build error. Explicitness over defaults.
3. **Unreliable intents are a first-class cell** — same envelope, same
   dispatch door, same distrust; fire-and-forget delivery for
   freshest-wins asks, with the staleness window carrying the ordering
   burden.
4. **Handlers are plain free functions with one fixed signature**
   (`void(NetContext &, const T &)`), bound fully-qualified with
   signature casts and a cross-module uniqueness check — zero runtime
   name lookup, zero same-name ambiguity.
