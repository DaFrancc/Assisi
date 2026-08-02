# Replication opt-in plan v1 — capability, policy, intent

**Status: PROPOSED — review passes folded in (see §7).**

This plan replaces the *opt-in model* of docs/replication-plan-v4.md (R1–R9,
built and green). It does not touch the transport, snapshot, baseline, or
body-state machinery — those survive unchanged. What changes is *who decides
what replicates, and where that decision lives.*

Informed by docs/replication-research-ecs-survey.md. There is **no migration
constraint**: the opt-in model is being defined for the first time, so v4's
fused model is replaced, not grandfathered. The goal is the best system, judged
by one criterion: mistakes we bake in here are architectural and expensive;
mistakes at the edges are small fixes. Optimize for getting the *shape* right.

---

## 0. The problem being solved

Today `ACOMP(replicated)` on a type is simultaneously three different claims:

1. **Capability** — this type's fields have a defined wire form.
2. **Policy** — every instance of it, on every marked entity, in every game
   this engine ever runs, goes on the wire.
3. **Cost** — the type is force-opted into change tracking everywhere,
   including single-player and the editor.

The fusion of (1) and (2) is the architectural defect. It surfaced concretely:
marking `Physics::Bounce` replicated — an edit made in an *engine* module to
serve *one test level* — silently became network policy for every future game.
An engine module cannot know any game's policy; under the fused model it is
forced to set one anyway.

The survey (docs/replication-research-ecs-survey.md) shows every mature
declarative system separates these: capability declared on the type, policy
decided per instance, with Unity NetCode, Overwatch, and SpatialOS as the three
independent implementations of the same split. The systems that fused them
(Godot: policy-only, no type level) or skipped declaration entirely (Unreal
Mass: imperative) are the outliers, with documented ergonomic costs.

---

## 1. The model: five gates, one intersection

A field value crosses the wire iff it passes **all** gates:

| # | Gate | Scope | Authored where | Mechanism | Polarity | Default |
|---|------|-------|----------------|-----------|----------|---------|
| G1 | Capability | Component **type** | Engine/module header | `ACOMP(replicable)` | Opt-in | Not capable |
| G2 | Game policy | **Game** | game.json | `networking.neverReplicate: [names]` | Opt-out of capable set | All capable |
| G3 | Entity gate | Entity **instance** | Level file / runtime | `Replicated` marker | Opt-in | Not replicated |
| G4 | Instance policy | Entity **instance** | Level file / runtime | `Replicated::excluded` | Opt-out of capable set | All capable |
| G5 | Field gate | Field (within type) | Component header | `AFIELD(norep)` | Opt-out | On the wire |

Plus the existing dynamic gates, unchanged: presence (the entity has the
component) and change (the acked baseline says the client lacks this value).
Five predicates, but only **three mechanisms** — annotation, config, marker —
each at the scope that owns the decision.

**Why each polarity is what it is:**

- G1 opt-in is universal across the survey; SpatialOS migrated *to* it after
  opt-out schema generation failed to scale. A type is not wire-capable until
  someone writes down that it is.
- G4 opt-out (default-all-capable, exceptions listed) is Unity's polarity: a
  `[GhostField]`-bearing component replicates on every ghost unless a per-prefab
  override removes it. The alternative — every instance lists what it sends —
  is Godot's fused model, whose survey entry documents the ergonomic grind and
  the 64-property mask ceiling. Four of our capable types (`Transform`, `Name`,
  `MeshRenderer`, `RigidBodyDescriptor`) are wanted on essentially every
  replicated entity; making every level author restate that would manufacture
  boilerplate and authoring mistakes in exchange for nothing.
- G2 exists so a *game* can narrow the engine's capability set once, instead of
  per entity. It is the direct answer to the `Bounce` incident at the correct
  scope: the engine says "can", the game says "not in this game", no level
  edits required.

**Who may author what:** engine modules may only ever touch G1 and G5 — the
capability axes. G2 belongs to the game. G3/G4 belong to the level author or
runtime gameplay code. An engine module that wants to change what a game sends
has no mechanism to do it with, which is the point.

**Expansion must be visible.** The residual risk of default-all polarity is a
future engine module marking a new type `replicable`, at which point every
marked entity carrying that component starts sending it. Two fences bound
this: capability cannot conjure components onto entities (the entity must
already carry the type), and a new engine capability is a one-word header edit
that code review sees, with D10's table as the checklist. The third fence is
visibility: **the server
logs its effective replicated-component set (names) at construction, and the
Network panel displays it.** A session's capability surface is a fact you are
shown, not one you reconstruct by forensics. This is the cheap 90% of what an
allowlist polarity would buy, without its silent-under-replication failure
mode.

---

## 2. Decisions

Each decision names its alternatives and why they lost. These are the
architectural commitments; the stages in §3 are just their execution order.

### D1 — Rename `ACOMP(replicated)` → `ACOMP(replicable)`

The annotation must state exactly what it now means: *can* travel, not *does*
travel. Keeping the old spelling while changing the semantics would leave every
future reader to discover the difference the hard way. Six sites across five
headers use it — one of them the NetSync **test-support** header
(`TestNetComponents.hpp:27`), whose migration is load-bearing for the suite —
and reflectgen rejects the old spelling with a message naming the new one (a
deliberate tombstone, not silent acceptance — the difference between "unknown
flag" and "this flag changed meaning" is exactly the kind of trap this plan
exists to remove).

`AFIELD(norep)` keeps its name: "not replicated" remains literally true for a
field inside a replicable type. `AASSET(replicable)` remains rejected (assets
are not entities).

### D2 — Capability is two-state: `replicable`, or nothing

`ComponentMeta`'s `bool replicated` becomes `bool replicable` — a rename, not
a redesign. A type either declares a wire form or it does not; absence means
"does not travel," and where the absence is a *decision* worth recording — as
for `Camera`, whose mirrored `isActive` would steal the receiving client's
view — the record is a header comment, where every other design rationale in
this codebase lives.

A tri-state (`replicable` / `local` / undeclared, SpatialOS-style) was in the
first draft and was **rejected in owner review**, for a reason the draft had
exactly backwards. The tri-state's only consumer was an editor warning on
undeclared serializable components of replicated entities, and silencing that
warning demands an `ACOMP(local)` on every legitimately-local component type
that ever appears on a replicated entity. On a real entity — a player carries
input state, camera rigs, audio emitters, AI, UI hooks — local components
outnumber replicated ones severalfold, so the annotation burden grows with the
game while the warning's precision shrinks. That is declaring-your-negatives,
the precise anti-pattern G1's opt-in polarity exists to avoid, reintroduced
through a lint. The warning was also misattributed: the `Test.alvl` incident
was a missing *entity* marker, which a per-component warning inspects nothing
without — the R7 unmarked-dynamic-bodies heuristic is what catches that class.
The survey majority agrees: Unity, bevy_replicon, Unreal, and Overwatch are
all two-state opt-in with silence meaning "no"; only archived SpatialOS
carried a tri-state, and it served schema-generation cost control, not lint.

What covers the forgot-to-mark class instead: **targeted heuristics and
visibility, not blanket declaration.** The R7 warning (unmarked dynamic bodies
while hosting) already exists; the Network panel shows the session's effective
capability set (§1); further heuristics are added one at a time when a real
mistake pattern shows up, none of them needing grammar.

Validation (reflectgen hard-fails): `replicable` + `transient` contradictory
(nothing to encode — unchanged rule); `norep` legal only inside a `replicable`
component (unchanged rule, new spelling). `replicable` + `tracked` together
is **legal, silently** — not redundancy but two independent claims.
`replicable` implies `tracked` because delta replication is a question posed
to the change ticks (an untracked component answers "never changed" and would
replicate once at spawn, then go silent); an explicit `tracked` alongside it
records that *non-network* systems need the ticks too. The difference matters
at removal time: strip `replicable` from a component that declared only
`replicable` and tracking goes with it — deliberately, no warning; the
annotation said nothing else needed it — but strip it from
`ACOMP(replicable, tracked)` and the tracking the game depends on stands.
`Transform` is the live example: `PropagateTransforms` needs its ticks
whether or not anything networks.

### D3 — `replicable` still implies `tracked` (kept, with a written trigger)

An untracked component reports change tick 0 forever — it would replicate once
at spawn and go silent — so the implication is load-bearing. Declaring both is
legal and meaningful (D2): an explicit `tracked` alongside `replicable`
records a non-network need for the ticks, so a later removal of `replicable`
cannot silently strip tracking from systems that depend on it. The cost objection
(tracking imposed on non-networked contexts) was re-examined and found smaller
than first claimed: `tracked` is a general ECS feature already serving
`PropagateTransforms` and the editor, and the marginal cost is one integer
store per mutable access plus one tick lane per pool.

**Written trigger for revisiting:** if Chiara ever shows change-tick stamping
in a frame profile, the fix is lazy lane activation — maintain tick lanes only
once a replication session exists. This is viable *because* of an existing
protocol property, verified at `Replication.cpp:544-547`: a new connection's
first snapshot deltas against baseline 0 (full state), so no pre-session tick
history is ever needed. Recorded here so the future fix is an optimization, not
a redesign.

### D4 — Instance policy lives on `Replicated`, as an exclusion list

```cpp
ACOMP()
struct Replicated
{
    AFIELD(min = 0.0, max = 100.0) float priority = 1.f;

    /// Capable component types this entity does NOT send: a bitset indexed
    /// by *replicable ordinal*, auto-sized at build time in byte steps (D5)
    /// — a single trivially-copyable byte at today's five engine types, no
    /// heap. On disk an array of component type NAMES (neither ids nor
    /// ordinals are build-stable), converted at the codec boundary. Empty =
    /// send all capable components present — the correct default for the
    /// four types wanted on essentially every replicated entity.
    AFIELD() Core::Reflect::ComponentMask excluded;
};
```

Why here and not elsewhere:

- **Not a bool inside each component** (the original option B): costs a branch
  per component per entity per snapshot, taxes every capable type with a
  boilerplate field, and the bool itself would need `norep` to stay off the
  wire. Rejected.
- **Not duplicate types** (`Foo`/`ReplicatedFoo`): no surveyed system does
  this; it doubles the type surface and every consumer needs both paths.
  Rejected.
- **Not a separate policy component**: `Replicated` *is* the entity's
  replication declaration — gate, priority, and now policy are one authored
  unit. A second component would split one decision across two places.
- **Forward-compatible with blueprints**: when the blueprint system is rebuilt,
  a blueprint is a serialized entity template *including* `Replicated` — so
  per-blueprint policy (Unity's per-prefab override, the majority mechanism)
  falls out with zero additional design.

`Replicated` stays plain `ACOMP()` — untracked. The first draft made it
`tracked` to invalidate a server-side name-resolution cache; with the mask
(D5) there is nothing to cache and nothing to invalidate. The server reads
`excluded` and `priority` live each snapshot, and the presence diff and D11
both compare live state against the acked baseline — none of it consults a
change tick, so a policy edit takes effect on the next snapshot through any
write path, and the ECS's untracked-write trap (`Scene.hpp:196-197`) has no
victim here.

### D5 — Policy is a bitmask in memory, names on disk

`Core::Reflect::ComponentMask` — `std::array<std::uint8_t, kReplicableMaskBytes>`,
a bitset indexed by **replicable ordinal**: a type's position in the sorted
list of replicable `ComponentId`s, computed once at registry finalize and
exposed as a dense `ReplicableOrdinalOf(id)` map. A split representation,
adopted in owner review to replace the first draft's
`std::vector<Core::ShortString>`:

- **In memory:** one byte per eight replicable types (a single byte today),
  trivially copyable, zero heap. `Replicated` is a core struct living in ECS
  pools that scene snapshots, PIE world builds, and undo capture all copy; a
  heap-owning vector there was the wrong shape. And a bit test is the natural
  operation for every runtime consumer — the write loop, the removal diff,
  D11, the `bodied` predicate — each one dense-array hop
  (`mask.test(ordinalOf[id])`), no search.
- **On disk:** an array of component type **names**. Neither ids nor ordinals
  are build-stable — ids are *alphabetical dense ids* (`ComponentMeta.hpp:122`
  — the registry sorts by name and assigns densely), and ordinals additionally
  shift whenever any type's capability flips — so persisting raw bits would
  silently re-aim an exclusion at the wrong component after an unrelated edit,
  the worst rot there is. Names rot exactly as fast as the level format itself
  (component blocks are name-keyed already), so no separate stable-id scheme
  is warranted.

The codec converts at the boundary, and the split has a precedent in the
reflection system: `EntityRef` is a raw handle in memory and a stable serial
index on disk. Save walks set bits → names via the registry; load resolves
names → bits via `ComponentRegistry::Find` / `IdOf(string_view)`
(`ComponentRegistry.hpp:37,66`). Policy identity depends on name uniqueness,
so the registry's duplicate-name rejection is promoted from "log and keep
first" to a hard invariant.

**Width is automatic — no cap to hit, no knob to set.** reflectgen already
parses every `ACOMP` in the build; an aggregation pass counts the replicable
types across all modules (engine and game alike) and emits
`kReplicableMaskBytes = max(1, ceil(count / 8))` into a generated header.
Byte granularity on purpose: the worst case wastes 7 bits instead of 63, and
byte boundaries give rebuild hysteresis — going from 8 to 9 replicable types
resizes the struct; 9 to 10 does not. Three properties make auto-sizing safe:
the width never serializes (disk and wire both speak names, so growth touches
no data and no protocol hash); the whole tree builds together (a resize is a
rebuild, not an ABI event); and the registry finalize fence survives as an
**internal consistency check** — actual replicable registrations must fit the
generated capacity, so a module the aggregation pass failed to scan (the only
way for it to be wrong) hard-fails at startup instead of writing bits past
the mask. If dynamically-loaded modules ever exist, build-time aggregation
stops being sound and this decision is revisited that day. Side benefit of
the index space: excluding a non-replicable type is *unrepresentable* (it has
no ordinal), so the invalid state cannot exist in memory at all — the load
boundary warns and drops it, per below.

**What the mask deletes from the first draft:** the server's per-NetId
`PolicyCache` and its invalidation, the equality early-out for priority-only
edits, the headless-retirement trap, `Replicated`'s `tracked` requirement,
and the ShortString name-length fence. There is no resolution step at runtime
at all — the mask *is* the resolved form.

**A typo must still be loud, and it moves to the load boundary.** An exclusion
name that resolves to no registered component, or to one that is not
`replicable`, warns at level load and drops the bit. The honest trade against
the vector form: an unresolvable name cannot persist through a load→save
round-trip (there is no bit to represent it), so "renders red forever"
becomes a load-time warning instead. Acceptable because the editor UI cannot
produce a typo — checkboxes only toggle registered components — so
unresolvable names arise only from hand-edited files or component renames,
both caught by the load warning at the first moment anyone can act on them.

### D6 — Excluding `RigidBodyDescriptor` means "visual-only mirror", and it is a feature

The body-state path (plan-v4 §3) keys off the descriptor, so excluding it has
system-wide meaning that must be defined, not discovered:

An entity whose policy excludes `RigidBodyDescriptor` is **non-bodied for
replication purposes**, whatever the server's physics is doing with it:

- `CaptureBodyStates` skips it **and erases any existing `_bodyStates` record
  for it.** The erase is not hygiene: the retirement sweep only removes records
  for retired NetIds, so a frozen record would outlive the exclusion with a
  nonzero tick that beats every post-sweep empty baseline — the stale body
  state resent to every client after every keyframe sweep, forever, and the
  stale tick would suppress the "first sighting" capture on re-inclusion.
- `WriteBodyStates` never mentions it.
- The `bodied` predicate (both call sites, `Replication.cpp:529-532` and the
  body pass) consults policy, so its `Transform` **replicates normally**
  instead of being suppressed — the mirror becomes an interpolated visual,
  rendered ~two snapshot intervals in the past like any non-bodied mirror.
- The client needs no policy knowledge at all: no descriptor ever arrives, so
  `SyncMirrorBody` and `ApplyBodyState` already take their null-descriptor
  early-outs (`Replication.cpp:1191-1194`, `:1238-1240`). The steady-state
  client behavior *falls out of the existing code*.

This is a genuinely useful authoring option, not an edge case to tolerate:
"replicate this as a visual, don't simulate it on clients" — debris the server
simulates but clients only watch, at interpolation cost instead of Jolt cost.

**The cost contract, stated honestly (both halves found in review):**

- *Rendered host:* the physics→Transform writeback stamps **every** dynamic
  body's Transform every frame, sleeping included (`PhysicsWorld.hpp:192`,
  `PhysicsWorld.cpp:726`). A resting excluded-descriptor entity would therefore
  resend its Transform block every snapshot forever — forfeiting, per excluded
  entity, the idle-bandwidth property the body channel was built for. **The
  chosen remedy is at the root:** the writeback skips the `GetMut` stamp when
  the body's pose is unchanged since the last write (one vec3+quat compare).
  This also removes a false-positive dirty for *every* resting body
  engine-wide — strictly less work everywhere. If implementation turns up a
  consumer that depends on the every-frame stamp, fall back to documenting the
  resend cost in this section; but none is expected, and the fix is the kind of
  no-op-write suppression the survey's tier-3 systems (SpatialOS's explicit
  dirty bit) exist for.
- *Headless host:* no writeback runs at all — the Transform component never
  receives the physics pose, and a visual-only mirror **freezes at load pose**,
  the exact pre-R5 bug class reintroduced for excluded entities. The contract:
  a world hosting D6 entities must run a physics→Transform writeback. The
  server warns (once per session) if a descriptor-excluded replicated entity
  exists and the hosting world has no writeback configured.

**Mid-session transitions must be specified** (they are reachable the moment
policy is runtime-writable):

- *Excluding mid-session:* the presence diff sends a `RigidBodyDescriptor`
  removal. The client's removal handling must grow one case: removing the
  descriptor from a mirror that has a live body tears the body down
  (`DestroyMirrorBody`, remove the transient `RigidBody` component — whose
  survival would also block any future rebuild, since `SyncMirrorBody` keys on
  it at `:1196`), and the entity re-enters `_transformHistory` interpolation.
  Without this the Jolt body outlives its authority — the invisible-obstacle
  bug class. `EnforceSleep` and `CaptureTransforms` recover on their own once
  `_bodies` loses the entry.
- *Un-excluding mid-session:* **works only via D11**, the presence-vs-acked
  force-send. Review of the first draft proved the "it just works" claim false:
  re-inclusion changes the *policy*, not the component, so the component's
  change tick predates the client's baseline and the write-loop gate
  (`Replication.cpp:547`) skips it until the next keyframe sweep — up to 8.5 s
  of a mirror the server believes is whole and is not. With D11, the descriptor
  is force-sent as full state the moment presence reappears, and the existing
  spawn path (`SyncMirrorBody` builds the body, erases transform history)
  handles the rest.

### D7 — Game-scope policy is a config filter, applied per session

`game.json`:

```json
"networking": {
    "neverReplicate": ["Bounce"]
}
```

The list is plumbed through **`ReplicationConfig`** — the constructor does not
read the filesystem; the session-owning layer loads config and fills the struct,
which keeps the server testable without files and consistent with how it
already receives everything else. Filtering happens once at construction,
against `_replicatedComponents`. Unknown names warn (same rule as D5).

Lifecycle, stated because it differs from precedent: quantization is loaded
once per *process* at startup; G2 is loaded per *session* (each
`NetSession::Host` constructs a server). A game.json edit therefore takes
effect on the next hosted session without a restart — deliberate, and safe
precisely because G2 is not in the hash.

Server-side only, and deliberately **not** in the protocol hash. The
distinction that makes this right, since quantization (also game.json) *is*
hashed: quantization changes how bytes **decode** — a mismatch is silent
corruption, the one thing a handshake exists to prevent — while G2 only changes
which self-describing blocks are **sent**. The receive side is provably
indifferent: `ApplySnapshot` checks only `meta->serializable`
(`Replication.cpp:1099-1105`), so a client whose own config lists Bounce still
applies an arriving Bounce block correctly. Two builds differing only in this
list pair fine; the server's list governs what is sent. Putting it in the hash
would make a config edit refuse connections for no correctness gain.

Footnote for the future: clients send no component data today (the server
accepts only Hello/Ack/Input/RequestKeyframe), so G2 needs no receive-side
enforcement. If transferable authority ever lands, G2 grows a server-side
receive filter the same day. Recorded in §6.

### D8 — The wire *format* does not change; the hash story, stated precisely

Nothing in this plan alters the byte layout of any component block. But the
protocol hash is affected twice, in ways the first draft got wrong, and the
stages must handle deliberately:

- **The capability flag is already a hash input** — v4's R1 put it there
  (`BinaryCodec.cpp:537` folds `" replicated"` / `" local"` per component into
  the hashed layout text, and `TestBinaryCodec.cpp:573` asserts that flipping
  it changes the hash). The emission is already exactly two-state, so P0's job
  is only to rename the flag it reads — `replicable` → `" replicated"`,
  absence → `" local"` — keeping the hash text byte-identical. A pin test
  captures the hash constant **before** the rename lands, or it pins nothing.
- **P2 changes the hash value.** `Replicated` is a serializable registry
  component; adding `excluded` adds a field line to its hashed
  description. `kNetProtocolVersion` stays 3, but builds straddling P2 will
  refuse to pair — correct behavior, stated here so it is not a mystery. The
  P0 pin is re-pinned at P2, and P2's DoD says so.

The operative architectural claim survives review and is worth keeping: the
client never infers "replicated entity ⇒ all capable components." It
materializes only what blocks actually deliver
(`Replication.cpp:1093-1133`), with presence tracked against the *acked
baseline* — which is exactly the property that lets policy change which blocks
appear without any protocol change.

### D9 — Excluding `Transform` is legal, with a contextual warning

A pure-data entity (replicated game state carrying no spatial meaning) may
legitimately exclude `Transform`. But an entity with a `MeshRenderer` or
`RigidBodyDescriptor` and no replicated `Transform` produces a mirror stuck at
the level-file pose — almost certainly a mistake. The editor warns when
`Transform` is excluded *and* a placement-dependent component is present; the
server honors the authored policy either way. The system does not silently
override authored data; it says why the data looks wrong and does what it was
told.

One consequence is decided rather than discovered: a **bodied** entity with
`Transform` excluded is also treated as non-bodied for capture (D6's rule,
same predicate). The client can never build its body — both build paths
require a Transform (`Replication.cpp:1192-1194`, `:1238-1240`) — so sending
body states it must drop would be pure waste.

### D10 — Every serializable engine component's status is a reviewed decision

Under D2's two-state model there is nothing to *declare* for a non-replicating
type — but the decision still deserves a record, and its home is the header
comment. P0 executes the sweep; this table is its checklist:

| Component | Decision | Why (recorded in the header comment) |
|---|---|---|
| `ECS::Transform` | `replicable` | Already; the universal pose channel. |
| `Runtime::Name` | `replicable` | Already; identity for debugging/UI. |
| `Runtime::MeshRenderer` | `replicable` | Already; what a mirror looks like. |
| `Physics::RigidBodyDescriptor` | `replicable` | Already; what a mirror's body is. |
| `Physics::Bounce` | `replicable` | Stays capable — G2/G4 now make that safe. |
| `Runtime::Camera` | not replicable | The founding incident: mirrored `isActive` steals the client's view. |
| Light components (all three) | not replicable | Flipping to `replicable` later is one word + regen — but note it changes the hash (D8), so mixed builds refuse to pair. Until a game wants networked lights, the minimal capability surface wins. |
| `Runtime::Parent` | not replicable | The hierarchy component (there is no separate `Children` type). Hierarchy replication is its own future project — v4 stripped hierarchy semantics deliberately. |
| `NetSync::Replicated` | *(plain `tracked`)* | The gate itself; never on the wire (D8). |
| `NetSync::Mirrored`, `Physics::RigidBody`, `Runtime::DestroyTag` | *(transient)* | Session/runtime state; unreachable by definition. |

There is no enforcement test: under two-state, absence is the default, not a
violation. What keeps a future engine capability honest is that granting one
is a one-word header edit reviewed like any other — and the session's
effective capability set is displayed, not inferred (§1).

### D11 — The presence-vs-acked force-send (the removal diff's dual)

New mechanism, added in review, and general rather than a D6 special case: in
the write loop, any component **present now but absent from the client's acked
component slice** is sent as a full-state block, bypassing the change-tick
gate. The `ackedLow`/`ackedHigh` range is already computed for the removal
diff (`Replication.cpp:502-505`); this is its mirror image.

Why it must exist: the change tick answers "has this value changed since the
client last saw it," but re-inclusion (G4 edit), and any future case where the
server knows the client lacks a component whose value never changed, are
questions about *presence*, not value. The removal diff already handles the
disappearing direction by comparing acked-vs-now; the appearing direction was
simply missing, papered over until now by the keyframe sweep. With D11, the
sweep returns to being insurance (v4 §its own words) rather than a load-bearing
delivery path for policy edits.

Cost: for entities in steady state the presence set equals the acked slice and
the check is a binary search that fails fast; no bytes change. When it fires,
it fires instead of a multi-second stall.

---

## 3. Stages

Each stage is independently landable and leaves every existing test green.
DoDs distinguish what a terminal can verify from what needs eyes, per the v4
convention.

### P0 — Annotation grammar and the `replicable` flag (reflectgen + Core)

- Capture the current `NetProtocolHash()` value as the pin constant **before
  any change lands** (D8).
- `reflect_codegen.py`: accept `replicable`; **reject** `replicated` with a
  message naming the rename; validations per D2.
- `ComponentMeta`: `bool replicated` → `bool replicable` (rename only). All
  readers updated — including `BinaryCodec.cpp:537` (whose two-state emission
  keeps the hash text byte-identical, D8) and `Replication.cpp:99-103`.
- Header rename sweep: five headers, six sites, **including
  `TestNetComponents.hpp:27`** (the suite depends on it). `Transform`'s
  `ACOMP(replicated, tracked)` becomes `ACOMP(replicable, tracked)` — the
  explicit `tracked` stays, correctly recording that `PropagateTransforms`
  needs the ticks independently of networking (D2).
- D10 comment sweep: each not-replicable row's rationale lands in its header
  comment.
- Test updates: `TestBinaryCodec.cpp:573` (flag-flip hash test, new spelling),
  the reflectgen grammar/rejection tests, golden regeneration (own commit, per
  §5).
- **DoD (terminal):** all reflectgen + Core + NetSync tests green; the hash
  pin test passes — i.e. P0 provably did not move the protocol hash.

### P1 — `FieldType::ComponentMask`

- `Core::Reflect::ComponentMask` (auto-sized byte-array bitset over
  replicable ordinals, D5) plus its `FieldType`, end-to-end. Follows the two existing precedents: for
  the memory-vs-disk split, `EntityRef` (raw handle in memory, stable serial
  on disk); for the vector-of-strings codec shape, `AssetPathVector`
  (verified present end-to-end: `reflect_types.py:146-160`,
  `BinaryCodec.cpp:240-247`, `:326-335`, `FieldMeta.hpp:33`). `FieldType`
  enum value appended (no existing index shifts), reflectgen recognition,
  JSON codec as an array of component names — registry-resolved in both
  directions, with D5's load-time warning — and field-meta goldens.
- Binary codec form is **names too** (varint count + strings, hostile-input
  caps like every other read) — deliberately not raw bits: the protocol hash
  covers *serializable component layouts*, not the full registry, so two
  builds that hash equal can still differ in id-only registrations and hence
  in id assignment. Raw bits would misalign there; names cannot. Cold data on
  a component that never crosses the wire anyway — size is irrelevant,
  correctness is not.
- **The auto-sizing pass**, in four pieces, each with precedent in this repo:

  1. **Collect** — `assisi_reflect()` appends its resolved header paths to a
     global `ASSISI_REFLECTED_HEADERS` property, one line beside the
     accumulation it already performs (`AssisiReflect.cmake:114` appends to
     `ASSISI_REFLECT_OBJECT_TARGETS`). Every reflected header in the tree
     routes through this function, so coverage is automatic rather than a list
     someone maintains.
  2. **Count** — reflectgen gains `--count-replicable <headers...> --out`. It
     already parses `ACOMP` arguments (`_check_replication()`), so this is a
     filter and a length, emitting
     `constexpr std::size_t kReplicableMaskBytes = max(1, (n + 7) / 8);`.
  3. **Emit** — one `add_custom_command` at top level, *after* every
     `add_subdirectory()` so the property is complete, writing to a temp and
     then `copy_if_different` into the generated dir (the idiom shader
     staging already uses, `apps/sandbox/CMakeLists.txt:117`). **The guard is
     load-bearing, not tidiness:** every header edit reruns the scan, but the
     file only *changes* when the count crosses a byte boundary, so below that
     nothing rebuilds at all. Without it, every header edit would rebuild the
     tree and the feature would be hated.
  4. **Consume** — `ComponentMask` lives in its own leaf header
     (`Core/Reflect/ComponentMask.hpp`) that includes the generated limits.
     Dedicated deliberately: only `NetComponents.hpp` and the codec include
     it, so when the width does change, recompiles are bounded to those TUs
     rather than to everything that touches Core.

  No target cycle exists even though the scanned headers live in modules that
  link Core: the aggregation depends on header **files**, not on module
  targets. Out-of-tree game modules must route their headers through
  `assisi_reflect()` to be counted — the same prebuilt-engine trigger D5 names.

  Consequence worth knowing rather than discovering: test-support headers
  carry `ACOMP(replicable)` too (`TestNetComponents.hpp`'s `Health`), so a
  test build counts one more replicable type than a release build and
  `ComponentMask` may differ in size between configurations. Correct rather
  than a bug — those components genuinely register and need ordinals — and
  harmless precisely because the width never serializes: disk and wire both
  speak names.

- Registry finalize gains the `ReplicableOrdinalOf` dense map — the existing
  `SerializableComponents()` pattern with a different predicate
  (`ComponentRegistry.cpp:60-99` already sorts by name and assigns dense ids)
  — and keeps the fence as the aggregator-vs-reality consistency check:
  registrations exceeding generated capacity hard-fail at startup, which is
  the only way the scan could be wrong (a module that escaped it).
- Inspector: generic rendering as a read-only name list (the policy UI in P4
  is custom, but the generic path must not crash on the new type).
- **DoD (terminal):** codec round-trip tests (JSON + binary); unknown-name
  load-warning test; truncation/fuzz test on the binary read path; a
  JSON-side malformed-array test (non-string element) matching the level
  format's hostile-file posture; consistency-fence test (a registration past
  the generated capacity fails loudly); golden diff reviewed (own commit).

### P2a — Instance policy: component exclusion, cache, force-send

The component half of the core, landable while P2b's body semantics are
argued.

- `Replicated` per D4 (mask field, stays untracked); level-format round-trip
  test.
- **D11 force-send** in the write loop.
- `WriteEntityComponents`: presence loop and write loop test the mask; the
  removal diff handles the exclusion direction (verified: no new wire
  machinery, `Replication.cpp:502-518`); D11 handles the re-inclusion
  direction.
- Effective-set construction logging per §1.
- **Tests (terminal):**
  - default-empty policy ⇒ byte-identical wire output to today (explicit
    wire-bytes comparison, plus the existing 77-case suite as the regression
    net);
  - excluded component absent from spawn and delta;
  - runtime exclusion ⇒ removal on client; **re-inclusion ⇒ full component
    state arrives on the next snapshot, not the next sweep** (this test fails
    without D11 — it is the review finding, pinned);
  - exclusion survives a keyframe sweep (the `sinceChangeTick = 0` full-state
    path consults policy too);
  - budget-skip carry-forward: an acked slice containing a newly-excluded pair
    converges to a removal once the entity fits;
  - policy reads are live: an exclusion flipped through a *plain query write*
    (deliberately untracked) still takes effect on the next snapshot — pins
    the no-tick-dependency decision (D4).
- **DoD (terminal, plus one stated hash fact):** all green; `NetProtocolHash()`
  **changes at this stage** (Replicated's new field enters the hashed layout,
  D8) — the P0 pin is re-pinned here, and the stage notes that builds
  straddling P2a refuse to pair, by design.

### P2b — Body-state policy semantics (D6, D9)

- `CaptureBodyStates`: skip + record-erase per D6; the shared "non-bodied for
  replication" predicate covering descriptor-excluded (D6) and
  Transform-excluded-while-bodied (D9).
- `WriteBodyStates` and the `bodied` predicate consult the same predicate.
- The writeback no-op-stamp suppression (D6's chosen remedy), in the Physics
  module, with its own test: a resting body's Transform change tick does not
  advance.
- Client: descriptor-removal teardown per D6 (destroy body, remove transient
  `RigidBody`, re-enter interpolation).
- Headless-host warning per D6's contract.
- **Tests (terminal):**
  - descriptor exclusion ⇒ no body records, Transform replicates, mirror
    interpolates; **at rest ⇒ zero recurring bytes** (proves the writeback
    remedy end-to-end);
  - mid-session exclusion ⇒ client body torn down — assert via the physics
    world's body count (no orphan Jolt body) and absence of the transient
    `RigidBody`;
  - re-inclusion ⇒ body rebuilt at the authoritative pose (rides D11);
  - `_bodyStates` record erased on exclusion — assert no body-state bytes for
    the entity after a keyframe sweep;
  - Transform-excluded bodied entity ⇒ no body records sent (D9);
  - `EnforceSleep` and transform-history sanity across the
    exclude→re-include cycle.
- **DoD (eyes):** none — deliberately; both P2 halves are fully
  terminal-verifiable, which is why they land before the editor stage.

### P3 — Game policy (G2)

- `ReplicationConfig` gains the list (D7); the session-owning layer fills it
  from game.json at host time; unknown-name warning; documented in game.json's
  `_comment` style.
- **Tests (terminal):** listed component absent from all snapshots for all
  connections; absent key ⇒ no change; malformed block warns and changes
  nothing (matching the quantization loader's contract); G2∩G4 overlap (both
  name the same component) is benign.

### P4 — Editor

- Inspector replication section, on an authoring (non-mirrored) `Replicated`
  entity: one checkbox per capable-and-present component; unchecking authors an
  exclusion. Edits go through the undo-capable path — verified viable: the
  restore path stamps change ticks (`AddComponentForRestore` bottoms out in
  `Scene::Add`, `EditHistory.cpp:297-311`), so undo of a policy edit
  invalidates the cache like any other edit.
- Components filtered by G2 render as **disabled** checkboxes with a "filtered
  by game.json" note — the author must not be handed dead switches.
- **Mirrors do not render the local marker's policy** — a mirror's
  `Replicated{}` is default-constructed by the client
  (`Replication.cpp:1057`) and the host's real marker was stripped from the
  joined world; showing it would display fabricated data. The mirror view
  derives from *observed component presence* instead ("receiving: Transform,
  MeshRenderer, …"), which is the only thing a client truthfully knows. A
  joined client's editor cannot author policy at all, and the UI says so
  rather than implying it.
- Warnings: `Transform` excluded alongside placement-dependent components
  (D9); D5's unresolvable-name warnings fire at level load, alongside the
  other load diagnostics. Aggregate counts and the effective capability set
  (§1) surface in the Network panel next to the existing R7 warnings.
- **DoD (eyes):** checkbox round-trip (uncheck → save → reload → still
  unchecked → mirror lacks the component live); warning appears/disappears as
  components are marked; undo restores a policy edit; disabled-G2 rendering.
  Terminal side: the editor-state tests for the inspector's replication
  section extend to the new rows.

### P5 — Documentation

- `Annotations.hpp` header comment rewritten around the five-gate table,
  including the `replicable`/`tracked` overlap note (D2): one change-tick
  lane, two readers; the explicit `tracked` is inert while `replicable` is
  present, and exists to preserve tracking if `replicable` is later removed.
  Documented here, deliberately not emitted at build time — a permanent note
  on correct code trains people to skim build output.
- plan-v4 §"opt-in" superseded-by note pointing here; this doc flips to
  **plan of record** status.
- The survey doc gains a short "what Assisi chose and why" postscript linking
  the decisions to their precedents.

---

## 4. Non-goals, with named seams

Declared so their omission is a decision, not an oversight — and so each has a
place to land when its trigger arrives.

- **Relevancy / interest management (per-client entity filtering).** The
  scaling axis every shipped system spends complexity on, and deliberately
  absent at PIE/co-op scale. **Seam:** a per-connection predicate in
  `SendSnapshot`, applied before priority accumulation; the Iris filter-stack
  (owner/connection/group/dynamic, filters strictly before prioritizers) is the
  reference shape. Nothing in this plan's gates touches per-connection state,
  so the seam stays clean.
- **Per-world policy.** The engine has multi-world support and `NetSession`
  binds one scene at construction. When a game needs per-world overrides, they
  live in the world profile and apply as a second constructor-time filter —
  same shape as G2, different owner. Named now so it lands as a decision, not
  a retrofit.
- **Per-field, per-instance policy.** No surveyed system offers it; `norep`
  covers the type-level field gate. If a real need appears it composes as a
  G4-style field list, but there is no evidence it ever will.
- **Value-compare / shared-quantized-state change detection.** Our change ticks
  share replicon's false-positive class (a `GetMut` that wrote nothing resends
  a component). P2b's writeback suppression removes the single worst instance;
  the general Iris model (quantize once, share across connections, diff there)
  is the eventual answer if bandwidth ever says so; the acked-baseline
  architecture doesn't preclude it.
- **Per-component send rate / reliability knobs.** Godot's coupling of mode to
  reliability is a documented trap; if cadence control is ever needed it
  belongs on the priority axis (G3's `priority` already steers frequency under
  pressure), not as a new gate.
- **Lazy change-tick lanes.** Deferred with a measured trigger; see D3.

---

## 5. Risks and open implementation details

- **Load-order assumption (D5/P1).** Name→bit resolution at level load
  requires a finalized component registry. Deserialization already requires
  metas by name, so this holds today; P1 adds an assert at the resolution
  site so a future init reorder fails loudly rather than resolving against a
  half-built registry.
- **Golden churn.** P0 and P1 both regenerate reflectgen goldens; land them as
  separate commits so a golden diff is reviewable against exactly one cause.
- **Writeback suppression blast radius (P2b).** The no-op-stamp skip touches a
  Physics-module behavior other systems observe through change ticks. Expected
  consumers (`PropagateTransforms`, render dirty-skip) only *benefit* from
  fewer false dirties, but the stage explicitly includes a sweep for consumers
  that might rely on the every-frame stamp before the change lands.

---

## 6. What this plan does not solve, honestly

Policy at the *entity* grain answers "which of this entity's components
travel." It does not answer "to whom" (relevancy, §4), "how often under
pressure beyond priority" (rate control, §4), or "who may write" (authority —
today the server owns everything and clients send no component data, which is
why G2 needs no receive-side enforcement; if lightyear-style transferable
authority ever lands, G2 grows a server-side receive filter the same day, and
that dependency is recorded here). Those are different questions with
different homes, and stapling any of them onto these gates would repeat the
original sin — one mechanism, three meanings.

---

## 7. Review record

One adversarial review pass (2026-08-02, fable agent, full code verification
against `Replication.cpp/.hpp`, `BinaryCodec.cpp`, `ComponentRegistry`,
`PhysicsWorld`, `EditorNet.cpp`, `EditHistory.cpp`, reflectgen). Verdicts:
D1/D2/D3 agreed; D4 agreed with the visibility requirement (§1); D5 amended
(id-assignment rationale corrected, capacity fence added, cache placement and
early-out specified); **D6 amended on three verified holes** (re-inclusion
provably stalled until the keyframe sweep → D11; the resting-body Transform
resend / headless freeze cost model → the writeback remedy and host contract;
stale `_bodyStates` records → erase-on-exclusion); D7 agreed with config
plumbing and lifecycle statements; **D8 corrected** (the capability flag was
already a hash input via v4 R1 — the work is mapping preservation, and P2a
knowingly moves the hash); D9 amended (bodied-with-excluded-Transform treated
as non-bodied); D10 corrected (`Runtime::Parent`, `Runtime::DestroyTag`; the
lights' later flip does affect pairing). Structural: P2 split into P2a/P2b;
mirror-side policy display changed to derived presence (the local marker on a
mirror is fabricated data); per-world seam named in §4. The review also
positively verified the claims the plan leans on: receive-side indifference to
G2, the removal diff needing no new wire machinery, the descriptor-null client
early-outs, the undo-path change stamping, and the AssetPathVector pattern
generalizing to P1.

A second pass (2026-08-02, owner review) **rejected the first draft's D2
tri-state.** The deciding argument: the tri-state's only consumer was the
undeclared-component warning, and silencing it demands an `ACOMP(local)` on
every legitimately-local component type that ever appears on a replicated
entity — a declare-your-negatives tax that grows with the game, the precise
anti-pattern G1's opt-in polarity exists to avoid. The warning's justification
was also misattributed: `Test.alvl` was a missing *entity* marker, invisible
to a per-component warning; the R7 heuristic is what catches it. The survey
majority (Unity, bevy_replicon, Unreal, Overwatch) is two-state opt-in;
archived SpatialOS alone carried a tri-state, serving schema-generation cost
rather than lint. D2 rewritten two-state; D10's enforcement test replaced by a
comment sweep; P4's undeclared warning dropped in favor of targeted
heuristics.

A third pass (2026-08-02, owner review) replaced the policy storage: the
vector of ShortStrings became `ComponentMask` — a fixed bitset in memory
(trivially copyable, no heap; `Replicated` is a core pooled struct), component
names on disk, converted at the codec boundary on the `EntityRef` precedent.
A follow-up ceiling question — what happens past the mask's width? — then
moved the index space from `ComponentId` to *replicable ordinal*: the cap now
binds the curated capable set (≤ 64 replicable types, registry-fenced,
raisable by recompile with no level migration) instead of every registered
component, invalid exclusions became unrepresentable, and the field shrank to
8 bytes.

A fourth pass (2026-08-02, owner review) tightened two things. reflectgen now
rejects `ACOMP(replicable, tracked)` as redundant — `replicable` implies
`tracked`, and the codebase's style is to make redundancy a build error
(`AFIELD(transient, norep)` precedent); `Transform` drops its explicit
`tracked` in P0 accordingly. And the mask width became automatic: a reflectgen
aggregation pass emits `kReplicableMaskBytes` at byte granularity (worst case
wastes 7 bits, not 63; byte boundaries give rebuild hysteresis), deleting the
manual knob entirely — the registry fence survives as an aggregator-vs-reality
consistency check, and dynamically-loaded modules are the named trigger that
would force revisiting build-time aggregation.

A fifth pass (2026-08-02, owner review) reversed the fourth's rejection of
`ACOMP(replicable, tracked)`: the pair is not redundancy but two independent
claims — implied tracking serves replication, while an explicit `tracked`
records that non-network systems need the ticks too, so a later removal of
`replicable` cannot silently strip tracking out from under them
(`Transform` / `PropagateTransforms` is the live case). The pair is accepted
silently, and the converse hazard — relying on *implied* tracking, then
removing `replicable` — is deliberately unguarded: the annotation said
nothing else needed the ticks. `Transform` keeps both words through the P0
rename, which also returns the generator to its current behavior (the pair
already builds today).

A sixth pass (2026-08-02, owner review) designed the auto-sizing concretely
after an objection that the work was smaller than the plan implied — correct:
`assisi_reflect()` already accumulates into a global property
(`AssisiReflect.cmake:114`), `copy_if_different` is already idiomatic here
(`apps/sandbox/CMakeLists.txt:117`), and `ReplicableOrdinalOf` is
`SerializableComponents()` with a different predicate against a registry that
already sorts and densely numbers every component
(`ComponentRegistry.cpp:60-99`). A fixed generous constant plus the runtime
fence was weighed as the zero-infrastructure alternative and declined in
favor of an exact count. The build-graph objection raised earlier (Core
depending on a scan of downstream headers) was withdrawn: file-level
dependencies create no target cycle, and a dedicated leaf header bounds
recompiles to the TUs that use the mask. P1 gained the four-piece design; the
two stale strings from earlier reversals (`NetIntent` in P0's title,
`excludedComponents` in D8) were corrected.
The consequence cascade deleted the server-side `PolicyCache` and its
invalidation, `Replicated`'s `tracked` requirement (policy is read live each
snapshot; no tick dependency anywhere), and the ShortString name-length
fence; typo diagnostics moved to the load boundary, accepting that an
unresolvable name cannot survive a load→save round-trip. P1 retargeted from
StringVector to ComponentMask, gaining the registry capacity fence and the
names-not-bits binary form (hash-equal builds may differ in id-only
registrations, so raw bits could misalign; names cannot).
