# How engines decide who is told about what — relevancy and ownership, a survey

*Research note, 2026-08-03. The third in the series after
docs/replication-research-ecs-survey.md (state) and
docs/replication-research-rpc-survey.md (events). Those two answered "what
travels" and "what just happened"; this one answers the two questions they both
deferred: **which entities each connection is told about at all** (relevancy /
interest management), and **what "ownership" of a networked entity actually
means** — one topic, not two, because in the dominant engine the relevancy
rules read the ownership chain. Every implementation claim carries its source
inline; anything inferred or unverifiable is marked as such. One standing
caveat stated once: **Unreal's engine source is not freely readable on the open
web** — every Unreal claim below comes from official documentation pages that
were actually fetched (marked "official"), Epic staff statements, or community
write-ups (marked "community"), never from reading the source. Quake 3, Source
SDK 2013, TNL, bevy_replicon, lightyear, and Flecs claims come from reading the
public source or rendered docs directly, with file/line citations. Where this
note and the earlier surveys would overlap, this one defers — read them
together.*

---

## 0. What is being asked, and why it is one question

Everything built so far in Assisi answers "what does the wire carry": five
gates decide which *components* may travel
(docs/replication-optin-plan-v1.md §1), the acked-baseline delta decides which
*values* travel, and the priority accumulator decides *what goes first* when a
snapshot does not fit. None of it answers **"to whom."** Today every ready
connection is told about every replicated entity; the opt-in plan names this as
a deliberate non-goal with a seam reserved for it: *"a per-connection predicate
in `SendSnapshot`, applied before priority accumulation"*
(docs/replication-optin-plan-v1.md §4), and plan-v4 §5 lists interest
management as "unstarted by design."

**Relevancy** (Unreal's word; also *interest management* — DIS/HLA and the
academic literature; *visibility* — bevy_replicon, Unity NGO, Godot;
*observers* — Mirror; *scope* — Tribes/TNL; *area of interest / AoI* — MMOs
and Photon) is the per-connection entity filter: for each connection, which
entities exist on the wire at all. It is a different axis from three things it
gets confused with, and the confusion is common enough that the distinctions
are worth stating up front:

- **Relevancy vs priority.** Relevancy is on/off — the client is or is not
  told the entity exists. Priority is how often an entity the client *is*
  told about gets updates under bandwidth pressure. Unity names the pair
  relevancy vs *importance*; Iris names it filtering vs *prioritization* and
  runs filters strictly before prioritizers; Assisi already has the priority
  half (the Tribes-lineage accumulator, `Replication.hpp:290-301`).
- **Relevancy vs dormancy.** Dormancy is "this entity has nothing to say for
  a while" — a property of the *entity's state*, the same for every
  connection. Relevancy is "this connection does not care" — a property of
  the *pair*. Unreal has both as separate mechanisms and people confuse them
  constantly; §5 takes this apart.
- **Relevancy vs the component gates.** The five gates decide what an
  entity's wire form *is*; relevancy decides which connections receive any of
  it. Orthogonal by construction — the opt-in plan's gates "touch no
  per-connection state" (its §4), which is what keeps the seam clean.

**Ownership** is the second half, and the user's question is the right one to
ask of an ECS: does ownership even mean the same thing there, and should it be
"an owner component or… implied through the systems that players write"? The
survey's answer, argued through §§14-16: the word is doing five unrelated jobs
in most engines — who may write authoritative state, whose input drives an
entity, where a directed message is delivered, what anchors relevancy, and
what gets client-side prediction — and the engines that fused them (Unreal
above all) pay for it in a table of gotchas that community documentation
exists to memorize, while the systems that split them (Photon Fusion, O3DE,
lightyear) have visibly fewer sharp edges. The ECS translation is not
"ownership becomes a component"; it is "each *job* becomes its own small
mechanism, and exactly one of them is naturally a component."

Assisi's position going in: server-authoritative with **no client→server
state channel at all** (plan-v4 §3.1 — clients send `InputCommand`s and
nothing else), per-connection acked baselines with per-entity ticks and a
despawn diff computed from the acked entity set (`Replication.hpp:270-309`),
a v1 joiner that is a spectator (plan-v4 §5), and an RPC design (survey §18)
whose recipient classes — *all-relevant*, *directed at the owner*, *except
the instigator* — presuppose exactly the two mechanisms this survey is about.

---

## 1. Quake 3 — the ancestor: presence in the snapshot is existence

The oldest system here and the cleanest statement of the model everything
else elaborates. All citations are the public source
(id-Software/Quake-III-Arena, master).

- **Scoping is per-client, per-frame, and geometric.** `SV_BuildClientSnapshot`
  "Decides which entities are going to be visible to the client, and copies
  off the playerstate and areabits" (`sv_snapshot.c:423-435`). From the
  client's eye position it fetches one decompressed PVS row —
  `clientpvs = CM_ClusterPVS (clientcluster)` — and the connected-areas mask
  (`CM_WriteAreaBits`), then walks **every** linked entity
  (`sv_snapshot.c:302-313`): no broadphase, O(clients × entities) per snapshot,
  each test a flags check, an area-connectivity check ("doors can legally
  straddle two areas, so we may need to check another one" — else
  "`continue; // blocked by a door`", `sv_snapshot.c:364-372`), and a bit test
  of the entity's cached cluster list against the PVS row
  (`sv_snapshot.c:374-386`).
- **The per-entity data structure is built at link time, not at snapshot
  time.** `SV_LinkEntity` recomputes an entity's PVS residency whenever it
  moves: "link to PVS leafs" via `CM_BoxLeafnums`, storing up to
  `MAX_ENT_CLUSTERS` (16) cluster ids plus a `lastCluster` overflow sentinel
  (`sv_world.c:282-332`, `server.h:34-46`). This is the cost model that
  made it fast in 1999: the expensive geometry query runs once per *move*,
  the per-client per-frame work is a bounded bit test.
- **The exceptions are explicit server flags**, not gameplay callbacks
  (`g_public.h:28-49`, comments verbatim): `SVF_NOCLIENT` "don't send entity
  to clients, even if it has effects"; `SVF_BROADCAST` "send to all connected
  clients" — bypasses area and PVS entirely; `SVF_SINGLECLIENT` "only send to
  a single client"; `SVF_NOTSINGLECLIENT` "send entity to everyone but one
  client"; `SVF_CLIENTMASK` — `singleClient` reinterpreted as a 32-client
  bitmask (`sv_snapshot.c:343-349`); `SVF_PORTAL` "merge a second pvs at
  origin2 into snapshots" — the portal-camera second pass literally recurses
  `SV_AddEntitiesVisibleFromPoint` from the portal's target point
  (`sv_snapshot.c:408-418`).
- **The client's own entity is never scoped in** — "never send client's own
  entity, because it can be regenerated from the playerstate"
  (`sv_snapshot.c:470-478`). The player is not an entity the player is told
  about; the player *is* the playerstate. This is ownership at its most
  degenerate, and §16 returns to it.
- **Dedup and overflow are one counter and one clamp.** `SV_AddEntToSnapshot`:
  "if we have already added this entity to this snapshot, don't add again"
  (the `snapshotCounter` stamp), and "if we are full, silently discard
  entities" at `MAX_SNAPSHOT_ENTITIES` = 1024 (`sv_snapshot.c:262-276`,
  `:228`).
- **Leaving scope is implicit for the game and explicit on the wire.** An
  irrelevant entity is simply absent from the new snapshot set; the delta
  encoder then tells the client so against its acked frame — "the old entity
  isn't present in the new message" writes an explicit remove
  (`MSG_WriteDeltaEntity(msg, oldent, NULL, qtrue)`, `sv_snapshot.c:104-109`)
  — and the client drops it from the frame ("entity was delta removed",
  `cl_parse.c:75-77`). cgame keeps a fixed `cg_entities[]` slot array and
  marks validity per snapshot (`cg_snapshot.c:147-151`), so **an entity that
  left the PVS and an entity that died are indistinguishable to the game
  code**. That is the failure mode and the feature in one: no stale ghosts,
  ever, at the price of no memory of what left.
- **Snapshot memory is the cost that scales with connections.** Per client, a
  `PACKET_BACKUP` ring of frames indexing into a global circular entity-state
  buffer sized `sv_maxclients * PACKET_BACKUP * MAX_PACKET_ENTITIES`
  (`server.h:155,202-204`); Sanglard's review of the model notes the price —
  "8 MB for 4 players" of retained gamestates
  ([Sanglard](https://fabiensanglard.net/quake3/network.php); his article
  covers the delta/ack machinery and, per the fetch, does not discuss the PVS
  filtering — the scoping story above is source-verified only).

## 2. Valve Source — a policy machine on top of the PVS, and clients that keep the body

Source inherits Quake's PVS and then does two genuinely new things: it turns
the per-entity transmit decision into a four-state *policy* machine with a
virtual-function escape hatch, and it changes what the **client** does when an
entity leaves scope. Citations are the public SDK
(ValveSoftware/source-sdk-2013, master, `src/` prefix) and the Valve wiki via
2024 Wayback captures (the live wiki rejects automated fetches).

- **The wiki states the model in one sentence**: "A player's PVS is usually
  used to filter entities before transmitted to the client, but more complex
  filter rules can also be defined in an entity's UpdateTransmitState() and
  ShouldTransmit() virtual functions"
  ([Networking Entities, Valve wiki via Wayback](https://developer.valvesoftware.com/wiki/Networking_Entities)).
- **Four edict states** (`src/public/edict.h:78-90`, comments verbatim):
  `FL_EDICT_ALWAYS` "always transmit this entity"; `FL_EDICT_DONTSEND` "don't
  transmit this entity"; `FL_EDICT_PVSCHECK` "always transmit entity, but cull
  against PVS"; and `FL_EDICT_FULLCHECK` "call ShouldTransmit() each time,
  this is a fake flag" — literally the absence of the other three, i.e. the
  slow path is the default-nothing case, and the wiki warns about it: "This
  creates lots of extra function calls, so only use it when needed."
- **`CServerGameEnts::CheckTransmit`** fills a per-client bit-vector
  (`CCheckTransmitInfo` carries the receiver's edict, its PVS byte array, and
  `CBitVec<MAX_EDICTS>` transmit bits — `src/public/iservernetworkable.h:37-55`)
  in a loop whose opening comment is a period piece: "for speed's sake, this
  assumes that all networkables are CBaseEntities and that the edict list is
  consecutive in memory… ideally we won't be calling any virtual from this
  routine" (`src/game/server/gameinterface.cpp:2482-2485`). The PVS test
  itself is cached like Quake's, but lazily: `RecomputePVSInformation()`
  rebuilds the entity's cluster list only when `FL_EDICT_DIRTY_PVS_INFORMATION`
  is set (`src/game/server/ServerNetworkProperty.cpp:144-151`), with a
  BSP-headnode fallback instead of Q3's lastCluster scan when the cluster list
  overflows (`:221-284` — which still carries the inherited Quake comment
  about doors straddling two areas).
- **Dependency chains transmit together.** `SetTransmit` "Force[s] our aiment
  and move parent to be sent" (`src/game/server/baseentity.cpp:4133`), and
  CheckTransmit's PVS-fail path walks the hierarchy upward looking for "any
  parent which is also in the PVS" before giving up
  (`gameinterface.cpp:2610-2667`) — including an honest in-source "BUG BUG"
  about unresolved two-parent cases. A child attached to a visible parent is
  visible; this is the same rule Unreal states as attachment relevancy (§4).
- **Default policy is derived from what the entity is.** The base
  `UpdateTransmitState`: an invisible entity with no move children ⇒
  `DONTSEND` (this is what silences logic entities); "Always send the world";
  skybox entities ⇒ `ALWAYS`; otherwise "by default cull against PVS"
  (`baseentity.cpp:3991-4026`). Weapons: "If the weapon is being carried by a
  CBaseCombatCharacter, let the combat character do the logic about whether or
  not to transmit it" (`basecombatweapon.cpp:132-145`), and the character
  sends the local player *all* his weapons but other players only the active
  one (`basecombatcharacter.cpp:2568-2596`). Viewmodels: owner ⇒ `ALWAYS`,
  everyone else ⇒ "Don't send to anyone else except the local player or his
  spectators" ⇒ `DONTSEND` (`baseviewmodel.cpp:58-98`) — Source's
  `SVF_SINGLECLIENT`, expressed as a virtual override.
- **Audio deliberately uses a bigger set than video.** Sound recipients are
  chosen by the PAS — *potentially audible set* — not the PVS:
  `CPASFilter` / `CPASAttenuationFilter` "Add players in PAS to recipient
  list" (`src/game/server/recipientfilter.h:137-158`), implemented by passing
  `usepas = true` to the engine's `Message_DetermineMulticastRecipients`
  (`recipientfilter.cpp:264-276`), with a distance/attenuation cull on top.
  `EmitSound` routes through it (`src/game/shared/SoundEmitterSystem.cpp:1133`).
  The design point: you hear the footsteps around a corner you cannot see —
  the relevancy set is *per medium*, an idea the academic literature had
  already formalized (§12's aura-per-medium). The engine-side PAS construction
  is closed source; verified at the game-DLL boundary only.
- **The client keeps the entity.** This is Source's biggest divergence from
  everyone else in this survey. When the server stops transmitting an entity,
  the client-side object is **not destroyed**: `NotifyShouldTransmit(
  SHOULDTRANSMIT_END )` → "We're no longer being sent by the server. Become
  dormant." — unlinked from the hierarchy, removed from the collision tree,
  hidden, but retained with its last state
  (`src/game/client/c_baseentity.cpp:2073-2134`); `SetDormant` is documented
  as "Flags this entity as being inside or outside of this client's PVS on
  the server" (`:4008-4023`), entities are *born* dormant until first
  transmitted (`:996`), and the wiki's `cl_entityreport` legend confirms the
  steady state: "red — Entity still exists outside PVS, but not updated
  anymore" (Networking Entities, via Wayback). Cost: client memory for
  everything ever seen, and visibly *stale* state if the game shows it.
  Benefit: re-entering scope is a delta against retained state, not a
  recreate — no pop-in of object identity, no loss of client-side-only state.
  Compare Unreal (§4), which destroys, and pays for it in exactly those two
  currencies.
- Also worth one line each: a debug convar exists to disable culling wholesale
  ("Will transmit all entities to client, regardless of PVS conditions" —
  `sv_force_transmit_ents`, `gameinterface.cpp:207`), HLTV/Replay bypasses PVS
  entirely ("for the HLTV/Replay we don't cull against PVS",
  `gameinterface.cpp:2577-2590`), and full snapshots exist only as bootstrap
  and loss recovery ("Usually full (non-delta) snapshots are only sent when a
  game starts or a client suffers from heavy packet loss" —
  [Source Multiplayer Networking, via Wayback](https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking)).

## 3. Tribes / TNL — scope as a per-connection query, priority as the degrade

The 1998 lineage that named the concepts most modern middleware uses.
Citations: the Tribes paper
([Frohnmayer & Gift](https://www.gamedevs.org/uploads/tribes-networking-model.pdf))
and OpenTNL's rendered docs.

- **Scope is per-connection and game-defined.** "the ghost manager does not
  ghost all objects in the simulation, but instead has a concept of 'scope.'
  … When an object comes into scope, its ghost is transferred to the remote
  host; when an object goes out of scope its ghost is deleted." And:
  "Scoping is the process of determining which objects on the server are
  relevant to a particular client. In the simplest sense, this means all
  objects that are potentially visible to a client from that client's current
  control object. This form of scoping is performed using a spatial database
  maintained by the simulation" (paper). TNL's API form: "Each
  GhostConnection has a scope object that is responsible for determining what
  other NetObject instances are relevant to that connection's client," with
  `performScopeQuery()` called "Each time GhostConnection sends a packet" and
  `objectInScope()` invoked per relevant object
  ([TNL::GhostConnection](http://opentnl.sourceforge.net/doxydocs/classTNL_1_1GhostConnection.html)).
  Note the anchor: scope is computed *from the connection's control object* —
  the first explicit statement in this survey of ownership anchoring
  relevancy.
- **Priority is computed by the object, against the scoping viewer.** "the
  object itself determines its own priority based on information such as
  current ghost state mask, distance from the scoping object, projected
  radius (using the scoping object's view frustrum parameters), relative
  velocity, animation state and interest modifiers (projectiles that are
  moving towards the client are more interesting than vehicles, vehicles are
  more interesting than items, etc.)" (paper). Packet fill is "ordered first
  by status change, then by object priority." Overflow degrades gracefully:
  low-priority objects just update less often — the accumulator Assisi
  already ships.
- **Per-(object, connection) state is explicit**: a Ghost Record with a Ghost
  ID and a per-state-bit mask — the ancestor of Assisi's per-connection
  `EntityBaseline` map. Escapes exist at both grains: "ghost always" objects
  (paper), and per-connection always-scope
  (`objectLocalScopeAlways()`, TNL docs).
- **Scope exit destroys the client object** — "onGhostRemove is called on the
  client side before the destructor when ghost has gone out of scope and is
  about to be deleted from the client"
  ([TNL::NetObject](http://opentnl.sourceforge.net/doxydocs/classTNL_1_1NetObject.html)).
  Tribes chose Unreal's answer, not Source's, a decade before either.

## 4. Unreal classic — relevancy on the owner chain, and a five-second grace

The system the question was asked about, and the one where relevancy and
ownership are inseparable by construction. Everything here is docs/community,
per the header caveat; "official" means the page was fetched.

- **The check order is documented, and ownership is checks one through
  three.** The tests "are implemented in the virtual function
  AActor::IsNetRelevantFor()", in order (official 4.27
  [Actor Relevancy and Priority](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-relevancy-and-priority?application_version=4.27),
  verbatim):
  1. "If the Actor is bAlwaysRelevant, is owned by the Pawn or
     PlayerController, is the Pawn, or the Pawn is the Instigator of some
     action like noise or damage, it is relevant."
  2. "If the Actor is bNetUseOwnerRelevancy and has an Owner, use the owner's
     relevancy."
  3. "If the Actor is bOnlyRelevantToOwner, and does not pass the first
     check, it is not relevant."
  4. "If the Actor is attached to the skeleton of another Actor, then its
     relevancy is determined by the relevancy of its base."
  5. "If the Actor is hidden (bHidden == true) and the root component does
     not collide then the Actor is not relevant."
  6. "If AGameNetworkManager is set to use distance based relevancy, the
     Actor is relevant if it is closer than the net cull distance."
  The distance is `NetCullDistanceSquared`, tooltip "Square of the max
  distance from the client's viewpoint that this actor is relevant and will
  be replicated" (official tooltip via the 4.27 Python API reference);
  default 225,000,000 = 15,000 units = 150 m (default value:
  [Neukirchen's compendium, community](https://cedric-neukirchen.net/docs/multiplayer-compendium/actor-relevancy-and-priority/)).
  So the *default* Unreal relevancy is: an owner-chain walk, three boolean
  escapes, an attachment rule, and a distance test — no PVS, no occlusion,
  geometry reduced to a radius.
- **The loop is per connection over a considered list.** From the official
  4.27 [Detailed Actor Replication Flow](https://dev.epicgames.com/documentation/en-us/unreal-engine/detailed-actor-replication-flow?application_version=4.27):
  build the considered list (skip `DORM_Initial`; skip actors below their
  `NetUpdateFrequency`; route `bOnlyRelevantToOwner` actors to their owning
  connection's list; call `PreReplication`), then per connection: relevancy,
  "Sort actors by priority", replicate until saturated. Priority is
  starvation-scaled — "An Actor with a priority of 2.0 will be updated
  exactly twice as frequently as an Actor with priority 1.0", and
  `GetNetPriority()` multiplies `NetPriority` by time-since-last-replicated
  (official relevancy page; accessor detail via Neukirchen). Defaults:
  Actor 1.0, Pawn 3.0, PlayerController 3.0.
- **The lifecycle has a grace period, then destruction.** Saturation rules
  from the same official flow page, verbatim: relevant for less than one
  second ⇒ "force an update next tick"; more than one second ⇒ re-check
  `IsNetRelevantFor`; and — the load-bearing line — **"If not relevant for 5
  seconds, close channel."** The modern property behind the 5 is
  `UNetDriver::RelevantTimeout` (API page exists but resisted fetch; the 5.0
  default is directly quotable only from the legacy BeyondUnreal wiki, which
  also states the *reason*: "a good balance between getting rid of
  non-relevant actors and not having to restart replication too often for
  actors that often switch between being relevant and being not" — community,
  legacy, snippet only). This is hysteresis-by-timeout: the channel does not
  slam shut at the relevancy boundary.
- **When the channel closes, the client destroys the actor** — for
  dynamically spawned actors. The official dormancy page states it by
  contrast: "Dormant actors are not checked for relevancy, so if a dormant
  actor would otherwise go out of relevancy on a client, it is not destroyed
  on that client" (official
  [Actor Network Dormancy](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-network-dormancy-in-unreal-engine))
  — i.e. a *non*-dormant one is. Placed-in-map replicated actors are the
  exception: they "don't get destroyed when net culled"
  ([WizardCell, community](https://wizardcell.com/unreal/multiplayer-tips-and-tricks/)).
  Re-relevancy is then a from-scratch respawn over a new channel with a full
  state send — "When an Actor becomes relevant to a connection, all the
  properties of said Actor are sent to the connection"
  ([vorixo, community](https://vorixo.github.io/devtricks/initial-dormancy/)).
  The failure modes are documented by the people who hit them: loss of
  client-side-only state and an expensive FastArray resync on
  re-entry, with community workarounds as invasive as filtering the
  considered list to avoid the channel close, and a warning that faking
  relevancy interacts badly with NetGUIDs
  ([UE forums thread, community](https://forums.unrealengine.com/t/stopping-actor-deletion-when-outside-of-network-cull-distance-on-clients/2097087)).
  This is the pop-in cost Source's keep-and-dormant model avoids, paid
  instead in client memory Source never reclaims.
- **Dormancy is the other axis, and the docs are explicit about the
  difference.** "While the actor channel for a replicated actor will close
  when it goes dormant, **dormant actors still exist on both the server and
  client**" (official dormancy page). The enum: `DORM_Never`, `DORM_Awake`,
  `DORM_DormantAll` ("dormant on all connections"), `DORM_DormantPartial`
  ("dormant on some connections, but not all"), `DORM_Initial` (all official,
  same page); waking is `SetNetDormancy(DORM_Awake)` or `FlushNetDormancy`,
  which "forces the actor to replicate at least one update… without actually
  changing its dormancy state." The property tooltip compresses the whole
  distinction into one clause: dormancy takes the actor "off of the
  replication list **without being destroyed on clients**" (official tooltip,
  4.27 Python API). So: *irrelevant ⇒ destroyed on that client; dormant ⇒
  kept everywhere, just silent.* The correctness cost is the discipline
  plan-v4 §3.3 already quoted: state must not change while dormant or the
  change may be lost — every mutation site must flush first. Assisi's sleep
  design was built as the structural version of dormancy (the transition is
  itself replicated and acked); §19 returns to why that means Assisi does not
  need dormancy as a mechanism at all.
- One community footnote worth keeping because it is a trap: a dormant
  actor's closed channel also means no Server/Client RPCs route to it until
  it wakes ([hzFishy, community](https://notes.hzfishy.fr/Unreal-Engine/Networking/Core/Dormancy-and-relevancy)).

## 5. Unreal at scale — the Replication Graph, then Iris

- **The problem, in Epic's own words**: "The standard network replication
  strategy, which is to require each replicated Actor to determine whether or
  not it should send an update to each connected client, performs poorly in
  cases like this and will bottleneck the server's CPU." The case: Fortnite
  Battle Royale "starts each game with 100 connected players and about 50,000
  replicated Actors" (both official 4.27
  [Replication Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/replication-graph?application_version=4.27)).
  Per-connection × per-actor evaluation is O(N·C) *virtual calls*, and at
  100 × 50,000 the evaluation itself — not the bandwidth — is the wall.
- **The fix is precomputed, shared lists.** Replication Graph Nodes "are
  responsible for building lists of Actors to replicate to each client on
  demand" and "do the actual work of establishing which Actors potentially
  require updates, sorting them into groups, storing precomputed lists to
  send to clients" (official, same page). Actors are routed into nodes once
  (on spawn/move), and the per-connection gather walks the node tree instead
  of testing every actor — a spatial 2D grid node for the world, always-
  relevant nodes shared by everyone, a per-connection node for owner-only
  actors, frequency-bucket nodes to stagger updates
  (node inventory:
  [Kieran Newland, community](https://www.kierannewland.co.uk/replication-graph-how-to-reduce-network-bandwidth-in-unreal/)
  — the official page names the classes in an example but does not describe
  them individually). The structural insight to keep: **relevancy stops being
  a per-pair predicate and becomes set membership in shared, incrementally
  maintained lists** — evaluate where things *change* (actor moved cells),
  not where they are *read* (every connection, every frame).
- **Iris replaces the graph with a filter stack plus prioritizers.** "The
  Iris Filtering System determines what objects are replicated to which
  connections," with four filter types, verbatim: Owner — "Object replicates
  to the same connections as its owner"; Connection — "Object replicates to
  specified, allowed connections…"; Group — "Object replicates to the same
  connections as all other objects in its group"; Dynamic — "Object
  replicates based on custom, dynamic filtering," with a shipped grid filter
  (`UNetObjectGridFilter`) among the built-ins (official
  [Iris Filtering](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-filtering-in-unreal-engine)).
  Prioritization is a separate stage: "Replication priority accumulates
  across network ticks until an object is replicated and its priority is
  reset," with 1.0 as the eligibility threshold (official
  [Iris Prioritization](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-prioritization-in-unreal-engine)).
  And the graph is gone: "Iris does not support the Replication Graph, since
  it has its own scheme for prioritizing and filtering objects"
  ([vorixo, community](https://vorixo.github.io/devtricks/iris-replication-filter/)).
  Note what survived every rewrite: owner-anchored filtering as a first-class
  filter type, filters strictly before prioritizers, and priority as an
  accumulator. Those three are the industry consensus distilled.

## 6. Unity Netcode for Entities — relevancy sets vs importance, named as such

The ECS system with the most explicit vocabulary for the axis split. All from
official Unity docs (com.unity.netcode@1.6 manual/API).

- **Relevancy is an explicit per-tick set of (connection, ghost) pairs.**
  `GhostRelevancy` is a server singleton — "Every frame, collect the set of
  ghosts that should be (or should not be) replicated to a given client" —
  whose `GhostRelevancySet` is "A sorted collection of (connection, ghost)
  pairs, that should be used to specify which ghosts, for a given connection,
  should be replicated (or not replicated, based on the GhostRelevancyMode)
  for the current simulated tick"
  ([GhostRelevancy API](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/api/Unity.NetCode.GhostRelevancy.html)).
  Polarity is selectable: `GhostRelevancyMode` = Disabled ("The default. No
  relevancy will be applied under any circumstances.") / SetIsRelevant
  (whitelist) / SetIsIrrelevant (blacklist), plus a query-shaped default
  (`DefaultRelevancyQuery`) that the explicit set overrides
  ([optimizations manual](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/optimizations.html)).
  The model is **imperative**: user systems rebuild the set each tick —
  maximum flexibility, cost proportional to churn, and the burden of spatial
  indexing left entirely to the game.
- **The client-side consequence is despawn, and the docs flag the
  confusion**: "If a ghost has been replicated to a client, then is set to
  **not be** relevant to said client, that client will be notified that this
  entity has been **destroyed**" — with the manual's warning that the
  despawn "does not imply the server entity was destroyed" (same page).
  Unreal's model, stated without the grace period.
- **Importance is the other axis and is documented as such.** Priority
  accumulates as base `Importance` × ticks-since-last-sent; "Once a packet is
  full, the server sends it, and all remaining ghost entities are simply not
  sent on this tick - though they are now more likely to be in the next
  snapshot" (same page) — the Tribes accumulator again, Assisi's existing
  mechanism. Distance-based importance scaling (`GhostDistanceImportance`
  with `GhostDistanceData` grid tiles partitioning ghost *chunks*) degrades
  update frequency by distance without ever making anything invisible — the
  soft version of relevancy, and notably the one Unity ships a spatial grid
  for, while hard relevancy gets none. Intended use, per the docs: "Use
  Relevancy to avoid replicating entities that the player can neither see,
  nor interact with" — distance limits, fog-of-war, per-client quest NPCs.

## 7. bevy_replicon — visibility as connection policy, then as filter components

The nearest-relative system (Assisi's marker/capability model already
parallels it — see the state survey), and its visibility API has lived two
lives worth recording separately because the migration is itself evidence.

- **Generation one: a policy enum plus an imperative per-client bitset.**
  `VisibilityPolicy` (through 0.36): All — "All entities are visible by
  default and visibility can't be changed"; Blacklist — "All entities are
  visible by default and should be explicitly registered to be hidden";
  Whitelist — "All entities are hidden by default and should be explicitly
  registered to be visible"
  ([docs.rs 0.32](https://docs.rs/bevy_replicon/0.32.0/bevy_replicon/server/enum.VisibilityPolicy.html)).
  `ClientVisibility` lives **on the connection's server-side entity**, not on
  replicated entities, with `set_visibility` / `is_visible`; changes are
  batched — hides are "queued for removal" and applied "in Self::update"
  once per replication tick (source,
  [client_visibility.rs v0.32](https://github.com/projectharmonia/bevy_replicon/blob/v0.32.0/src/server/client_visibility.rs)).
  When the policy is All, the component is not even inserted — zero cost when
  unused (CHANGELOG 0.32).
- **Generation two (0.37+, current): visibility as declarative filter
  components**, "which works similarly to layers in physics" (CHANGELOG
  0.37). A `VisibilityFilter` is a component on replicated entities; its
  `ClientComponent` is a component on client entities; `is_visible(&self,
  client: &ClientComponent) -> bool` is evaluated **by observers on component
  insert/remove — event-driven, not per-tick scan** (source,
  [visibility.rs](https://github.com/simgine/bevy_replicon/blob/master/src/shared/replication/visibility.rs);
  [ClientVisibility docs](https://docs.rs/bevy_replicon/latest/bevy_replicon/server/visibility/client_visibility/struct.ClientVisibility.html):
  "Stores only entities that have some hidden data. Automatically updated by
  observers"). Filters AND together, capped at `u32::BITS` registered
  filters; the `Scope` type parameter selects whether a filter hides the
  whole entity, one component, or everything except listed components
  (`AllExcept`, 0.41) — **per-client, per-component relevancy**, the only
  system surveyed with that grain besides SpatialOS. Revoke semantics are
  explicit: "If the [`VisibilityFilter::Scope`] was previously visible, it
  will be despawned (for entities) or removed (for components)" (source,
  server/visibility.rs). The doc example is a `Guild` component on both
  sides compared by equality — set membership, not geometry; spatial interest
  is left to the game.
- The migration's lesson: replicon started where Unity is (imperative
  per-client sets) and moved to *data on entities evaluated by the engine* —
  the SpatialOS direction (§9) — because Bevy grew the observer machinery to
  make it cheap. What your ECS can index determines what your interest
  system should look like.

## 8. lightyear — rooms, and the fully decomposed `Replicate`

The other Bevy stack (since 0.27 layered *on* replicon for transport/diffing
— see the state survey), and the system that most explicitly splits both of
this survey's questions into orthogonal parts.

- **Two interest mechanisms**: direct — a `RelevanceManager` with
  `gain_relevance(client, entity)` / `lose_relevance(client, entity)` (now
  `VisibilityExt::gain_visibility/lose_visibility`, cached until changed) —
  and **rooms**: "if a client is in a room but the entity is not (or
  vice-versa), we will not replicate… if the client and entity are both in
  the same room, we will replicate"
  ([book, interest management](https://cbournhonesque.github.io/lightyear/book/concepts/advanced_replication/interest_management.html);
  [visibility::immediate](https://docs.rs/lightyear_replication/latest/lightyear_replication/visibility/immediate/index.html)).
  Gated per entity by `VisibilityMode::InterestManagement` vs `::All`
  (default). Rooms are the MMO cell model (§12) as an API: membership is a
  set intersection, cheap and coarse, with the game deciding what a "room"
  means (a grid cell, a dungeon instance, a team).
- **The `Replicate` bundle separates four concerns that Unreal fuses**
  (verified at 0.17.1 —
  [docs.rs](https://docs.rs/lightyear/0.17.1/lightyear/prelude/server/struct.Replicate.html)):
  `target: ReplicationTarget` (who receives), `authority: AuthorityPeer` (who
  simulates and sends updates), `sync: SyncTarget` (who *predicts* vs
  *interpolates*), `controlled_by: ControlledBy` (which client controls it) —
  plus `relevance_mode`, grouping, hierarchy. Four ownership-adjacent axes,
  four fields. §15 leans on this heavily.
- **`ControlledBy` is the disconnect-lifetime anchor**: "Sender-side
  component that associates the entity with a [connection] 'controlling' the
  entity… When the link is disconnected, the sender will optionally (based on
  the `Lifetime` value) despawn the entity," with `Lifetime::SessionBased` —
  "When the client that controls the entity disconnects, the entity is
  despawned" — vs `Persistent`
  ([ControlledBy](https://docs.rs/lightyear_replication/latest/lightyear_replication/control/struct.ControlledBy.html),
  [Lifetime](https://docs.rs/lightyear_replication/latest/lightyear_replication/control/enum.Lifetime.html)).
  The receiver gets a `Controlled` marker.
- **Authority is separate from control and transferable at runtime**:
  "authority… is the decision of which **peer is simulating an entity**. The
  authoritative peer (client or server) is the only one that is allowed to
  send replication updates for an entity," "Only **one peer** can be the
  authority over an entity at a given time," transferred via
  `transfer_authority`, gated by a `HasAuthority` marker
  ([book, authority](https://cbournhonesque.github.io/lightyear/book/concepts/advanced_replication/authority.html)).
  This is the mechanism the opt-in plan's §6 flagged as the trigger for a
  server-side receive filter if Assisi ever adopts it.

## 9. SpatialOS — interest as data, authority as assignment (archival)

Dead as a product, but the cleanest architectural statement of both halves.
All quotes from web.archive.org captures of the official docs (canonical
domains are DNS-dead).

- **The two concepts are the system's own top-level split**: "This access is
  governed both by what the worker instance has authority over and what it
  has interest in." Authority: "SpatialOS only allows one worker instance to
  have write access authority to an entity's component at any one time."
  Interest: "For a server-worker instance to read the state of entity
  components, if it doesn't have write access authority over them, it needs
  to have interest"
  ([authority-and-interest, archived](https://web.archive.org/web/20210928045444/https://documentation.improbable.io/spatialos-overview/docs/authority-and-interest)).
  Write authority is per-component, per-entity, gated by a permission
  component (`EntityAcl`) and *assigned by the load balancer*: "The Runtime
  decides this area for each worker instance based on the load balancing
  strategy that you set up" ([write-access-authority, archived](https://web.archive.org/web/20210928043108/https://documentation.improbable.io/spatialos-overview/docs/write-access-authority)).
- **Query-based interest is a component containing queries.** "You can
  specify queries for component data by adding the improbable.Interest
  component to entities"; "A query contains a constraint and a result type.
  Constraints define which entities the query matches, and result types
  define the set of component updates the worker receives for the matched
  entities" ([query-based-interest, archived](https://web.archive.org/web/20210504162630/https://documentation.improbable.io/spatialos-overview/docs/query-based-interest)).
  Constraints compose: sphere/cylinder/box, *relative* variants anchored to
  the querier, entity-id, **component-presence** (`component_constraint` —
  team visibility expressed as "interest in everything carrying `RedTeam`"),
  and And/Or. Result types select a *component subset*, and each query
  carries an optional max `frequency` — "If multiple queries with different
  frequencies match the same entity component, the highest frequency
  applies" ([set-up QBI, archived](https://web.archive.org/web/20210928044255/https://documentation.improbable.io/spatialos-overview/docs/set-up-query-based-interest)).
  So "position only, at 0.5 Hz, at 200 m; full state at 20 m" is directly
  expressible as data, evaluated by the runtime, no callbacks anywhere. The
  cost warning is their own: full-frequency interest "can cause significant
  and unnecessary bandwidth load and CPU overhead. For example, a game
  client might unnecessarily receive updates about an enemy that is 200m
  away… at the same frequency as an enemy that is two meters away"
  ([GDK interest docs, archived](https://web.archive.org/web/20210928050011/https://documentation.improbable.io/gdk-for-unreal/docs/game-client-interest-management)).
- The idea to keep and the idea to leave: declarative interest-as-data is
  the right *authoring* surface (replicon's filters converge on it; §19
  borrows it); a fully general runtime query engine evaluating arbitrary
  constraints per pair is the expensive part, and nothing at Assisi's scale
  wants it.

## 10. The rest of the field, briefly

- **Photon Fusion** — the middleware with real AoI. "Interest Management is
  a set of data culling features, which restrict data replication of specific
  Network Objects and Network Behaviours from the Server to specific Player
  Clients." Per-object modes: Area Of Interest ("Players with an AOI region
  which overlaps this Network Object's position… will be interested"),
  Global ("always of interest to all Players"), Explicit (interest only via
  `Runner.SetPlayerAlwaysInterested()`); players register AoI regions with
  `Runner.AddPlayerAreaOfInterest` per tick; `IInterestEnter`/`IInterestExit`
  callbacks fire on transitions (with documented spawn/despawn caveats), and
  per-*property* culling exists server-side via `ReplicateTo(player)`
  ([Fusion interest management, official](https://doc.photonengine.com/fusion/current/manual/advanced/interest-management)).
- **Mirror** — a pluggable `InterestManagement` base class:
  `OnRebuildObservers(identity, newObservers)` fills the observer set per
  NetworkIdentity, and the source comment states the lifecycle: "Server will
  automatically spawn/despawn added/removed ones"
  ([InterestManagement.cs](https://github.com/MirrorNetworking/Mirror/blob/master/Assets/Mirror/Core/InterestManagement.cs)).
  Shipped implementations: Spatial Hashing ("one global Vis Range setting"),
  Hex Spatial Hashing, Distance, Scene, Match, Team
  ([Mirror docs](https://mirror-networking.gitbook.io/docs/manual/interest-management)).
  Note the roster: two spatial grids, a distance check, and three *set
  membership* filters — the shipping-middleware consensus on what games
  actually need.
- **Unity NGO** — a per-object server callback plus imperative show/hide:
  `CheckObjectVisibility(clientId) -> bool` "invoked when new clients connect
  or just before the associated NetworkObject is spawned"; hiding means "the
  client will despawn and destroy the NetworkObject"; `NetworkShow/Hide` and
  `SpawnWithObservers = false` round it out
  ([object visibility, official](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.6/manual/basics/object-visibility.html)).
- **Godot** — the same shape on the synchronizer: `public_visibility`,
  `set_visibility_for(peer, visible)`, `add_visibility_filter(callable)`
  taking a peer id and returning bool, with an explicit evaluation-cadence
  knob (`visibility_update_mode` = idle / physics / **none** — manual
  `update_visibility()` only)
  ([MultiplayerSynchronizer, official](https://docs.godotengine.org/en/stable/classes/class_multiplayersynchronizer.html)).
  A shipping engine deciding that *when the filters re-run* is the user's
  call is a small, good idea.
- **Overwatch** — the honest entry: **no findable primary or secondary source
  says whether Overwatch culls entities per client at all.** What the GDC
  material does document is per-*role* payload trimming — remote entities
  receive only what remote Statescript instances reference, measured at 2028
  bits local vs 806 bits remote for one ability sequence (Reed deck, via the
  state survey's verified reading) — which is the component/field axis, not
  the entity axis. At 6v6 on small maps, entity-level culling plausibly does
  not pay; that is inference, marked as such.
- **Photon Quantum** — the deliberate opposite, again: full-state
  determinism means every client holds everything; "client-controlled
  secrets used in a card game and Fog Of War-like features are easily
  hackable," Photon's own docs say (via the state survey's verified entry).
  Relevancy is not a feature Quantum lacks; it is a feature its architecture
  *cannot express*. Worth keeping as the boundary case: information scoping
  requires an authoritative node that knows more than clients do — Assisi's
  model has one; lockstep does not.

## 11. The literature — auras, cells, DDM, and what is dated

The academic thread is older than every engine above and got several things
right first. Primary sources were fetched for all of the following unless
marked.

- **Aura / focus / nimbus (Benford & Fahlén, ECSCW 1993)** — the founding
  vocabulary. "Aura is defined to be a sub-space which effectively bounds the
  presence of an object within a given medium and which acts as an enabler of
  potential interaction… when two auras collide, interaction between the
  objects in the medium becomes a possibility." Awareness is then negotiated
  asymmetrically: "The level of awareness that object A has of object B in
  medium M is some function of A's focus on B in M and B's nimbus on A in M"
  ([paper PDF](https://www.lri.fr/~mbl/ENS/CSCW/2013/papers/Benford_CSCW1993.pdf)).
  Two ideas here outlived the systems (DIVE, MASSIVE — the latter peaked
  around 10 users): interest is a *pairwise, possibly asymmetric* relation
  (my sniper scope sees you; you do not see me), and it is **per medium** —
  "you may be able to see me before you can hear me because my visual aura is
  larger than my audio aura" — which is exactly Source's PVS-vs-PAS split
  shipping in a commercial engine (§2).
- **The precise-vs-cheap taxonomy** (Boulanger, McGill MSc thesis, primary —
  the superset of the NetGames 2006 paper): proximity-based algorithms
  (Euclidean distance, square tiles, hexagonal tiles) vs visibility-based
  (ray visibility, tile visibility). Measured trade: pure pairwise distance
  "must compute the distance between all pairs of subscribers and
  publishers… O(|S||P|)… does not scale well"; square tiles "scale[] well as
  the complexity… is constant. However, it is a rather bad approximation";
  and the punchline for occluded worlds — tile-based path-distance gets
  message counts "closest to the ideal number (given by the Ray Visibility
  algorithm), but the computational effort required… is 3 to 6 times lower"
  ([thesis PDF](https://www.cs.mcgill.ca/~jboula2/thesis.pdf)). Liu's survey
  lineage adds the standing MMO fact: "Popular MMOGs like EverQuest, Final
  Fantasy XI, and World of Warcraft, all use zone-based interest management"
  — cheap, coarse, and dominant — while aura schemes are "much more precise…
  however… more computational effort is required for interest matching"
  ([Liu thesis PDF](http://etheses.bham.ac.uk/id/eprint/3710/1/Liu12PhD.pdf);
  the 2014 ACM CSUR survey itself was paywalled — taxonomy verified from the
  thesis). "Nimbus is also often referred to as Area-of-Interest (AOI)"
  ([Yahyavi & Kemme, CSUR 2013, mirror PDF](https://romisatriawahono.net/lecture/rm/survey/network%20security/Yahyavi%20-%20Architectures%20for%20Massively%20Multiplayer%20Online%20Games%20-%202013.pdf)).
- **DIS → HLA DDM — declarative interest, standardized.** DIS's baseline was
  broadcast-everything, and the numbers that motivated everything after:
  "for a simulation of 100,000 entities, each node in the network would
  require a 375 Mbps network connection. ESPDUs may account for 62 - 97% of
  the data traffic. In some experiments, as much as 90% of the data is
  useless to the receiving entity"
  ([Morse 1996, primary](https://escholarship.org/uc/item/9n9895jx) — also
  the source of the terminology footnote: "Interest Management is also
  referred to as relevance filtering and data subscription"). HLA's DDM:
  publishers create *update regions* and subscribers *subscription regions*
  in multidimensional routing spaces; "Two extents overlap if and only if
  they are of opposite type and all of their ranges overlap… In that case
  simulation data should be delivered"
  ([Petty 2002, primary](https://www.uah.edu/images/research/cmsa/pdf/Pubs_Dr_Petty/Petty%202002%20Comparing%20DDM%201.3%20%201516.pdf)).
  Publish-region × subscribe-region intersection is SpatialOS QBI twenty
  years early, and Fusion's AoI-region-overlap today.
- **RING (Funkhouser, I3D 1995)** — server-side precomputed cell-to-cell
  visibility culling updates: "update messages are sent only to workstations
  with entities that can potentially perceive the change," measured as "a
  40x decrease" in client-processed messages with 1024 entities in a densely
  occluded world ([paper PDF](https://www.cs.princeton.edu/~funk/symp95.pdf)).
  The direct academic ancestor of PVS-based relevancy, published four years
  before Q3.
- **Donnybrook (SIGCOMM 2008)** — the attention-based outlier. Its critique
  of AoI is the one worth keeping: "no such limit occurs naturally, because
  population density in real games follows a power law" — a radius does not
  bound the set when everyone stands in the same courtyard. Its answer: "a
  player's interest set, the set of other players to whom he is paying
  attention… always remains small due to the limits of human attention"
  (five, from a user study), scored by proximity, aim, and interaction
  recency; everyone else is approximated by locally simulated "doppelgänger"
  bots guided by 1 Hz summaries
  ([paper PDF](https://pages.cs.wisc.edu/~akella/CS838/F09/838-Papers/p389-bharambe.pdf)).
  900 players in simulation over consumer uplinks. Nobody shipped it; the
  power-law point stands anyway, and its modern echo is priority systems
  that key on view direction and recency (Tribes already did — §3).
- **What is dated, plainly.** (a) *Multicast-group-per-region* — the DDM
  deployment story — never reached games; Funkhouser already listed why in
  1995: join/leave latency, group-count limits, no WAN multicast. Its
  descendant is server-side grid cells feeding per-connection sets — Epic's
  RepGraph grid is NPSNET's hexagon-per-multicast-group with the multicast
  removed. (b) *P2P interest management* (Donnybrook, Voronoi overlays) died
  with the P2P model: "Generally, P2P approaches are not widely adopted in
  commercial systems… the challenge remains in the problem of maintaining
  control over the game and dealing with cheating" (Yahyavi & Kemme). (c)
  *Precise aura-pair matching* lost to cells/grids everywhere it competed —
  the O(n²) pair test is the thing every shipping system spends its design
  budget avoiding. (d) What the literature does **not** cover: the
  *lifecycle* half of this survey — grace periods, client-side retention vs
  destruction, re-entry state resend — is engine lore, documented nowhere
  academic that this pass found.
- MMO practice, for calibration: EVE runs one shard partitioned by solar
  system with hot systems pinned to dedicated hardware
  ([Game Developer / CCP](https://www.gamedeveloper.com/game-platforms/feature-ccp-outlines-single-shard-mmo-development);
  [High Scalability](https://highscalability.com/eve-online-architecture/));
  Second Life runs one process per 256 m × 256 m region
  ([official wiki](https://wiki.secondlife.com/wiki/Server_architecture)).
  Region-as-process is interest management promoted to deployment topology —
  far beyond anything Assisi needs, listed to bound the design space.

---

## 12. Comparison table — relevancy

| System | Unit of decision | Who computes it | Data structure | Recomputed when | Client on exit | Re-entry cost | Escape hatches |
|---|---|---|---|---|---|---|---|
| **Quake 3** | (client, entity) per snapshot | engine, geometric (PVS + areas) | per-entity cluster list (≤16) built at link time; per-client PVS row | entity: on move; test: every snapshot | entity gone (slot invalidated); indistinguishable from death | full resend from baseline | `SVF_BROADCAST` / `SINGLECLIENT` / `NOTSINGLECLIENT` / `CLIENTMASK` / `NOCLIENT` / portals |
| **Source** | (client, edict) per snapshot | engine PVS + game policy flags + `ShouldTransmit` virtual | 4-state edict flags; lazy cluster list; per-client transmit bitvec | cluster list on dirty; flags on `DispatchUpdateTransmitState`; FULLCHECK per check | **entity kept, marked dormant**, hidden/unlinked | delta against retained state | `FL_EDICT_ALWAYS`/`DONTSEND`; team hook; parent/aiment chains; PAS for audio |
| **Tribes/TNL** | (connection, object) per packet | game `performScopeQuery` vs spatial DB | per-pair Ghost Record (id + state mask) | every packet send | ghost deleted | re-ghost + full state | ghost-always; per-connection scope-always |
| **Unreal classic** | (connection, actor) per net update | engine walks owner chain + booleans + distance | none shared — per-pair virtual call | every considered actor, every connection, every update | actor **destroyed** (dynamic); placed-in-map kept | new channel, full property send, FastArray resync | `bAlwaysRelevant`, `bOnlyRelevantToOwner`, `bNetUseOwnerRelevancy`, attachment, dormancy (separate axis) |
| **Unreal RepGraph** | node membership | engine nodes (grid, always-relevant, per-connection) | precomputed shared actor lists | on actor move/spawn (route); gather per connection per tick | same as classic | same as classic | per-node policy; `AlwaysRelevant_ForConnection` |
| **Unreal Iris** | filter verdict | filter stack (owner/connection/group/dynamic incl. grid) | per-object filter handles; event-driven | on filter/state change | same as classic | same | filter types; prioritizers separate |
| **Unity N4E** | (connection, ghost) pairs | **game systems, imperative** | `GhostRelevancySet` hashmap, either polarity | rebuilt every tick by user code | ghost despawned ("notified… destroyed") | respawn + full snapshot | `GlobalRelevantQuery`; importance is the soft axis |
| **bevy_replicon** | per-client bitset / filter match | gen-1: game imperative; gen-2: engine observers over filter components | `ClientVisibility` masks on client entities; ≤32 registered filters | gen-1: batched per tick; gen-2: event-driven on insert/remove | despawn (or component removal — sub-entity grain) | fresh insert | policy polarity (gen-1); `Scope`/`AllExcept` (gen-2) |
| **lightyear** | per-pair relevance or room intersection | game (`gain/lose_visibility`) or room sets | cached relevance; `Rooms` on both clients and entities | on change (cached) | despawn | fresh replicate | `VisibilityMode::All` per entity |
| **SpatialOS** | query match, per component | runtime evaluates queries authored as data | `Interest` component: constraints + result types + frequency | runtime-continuous; queries updatable as component writes | checkout lost | re-checkout | component-presence constraints; per-query frequency |
| **Fusion** | AoI region overlap / mode | engine | per-player AoI regions; per-object mode | AoI re-registered per tick | interest lost (enter/exit callbacks) | re-interest | Global / Explicit / `SetPlayerAlwaysInterested`; `ReplicateTo` per property |
| **Mirror** | observer set per identity | pluggable `InterestManagement` | `observers` HashSet per NetworkIdentity | `OnRebuildObservers` on rebuild | auto-despawn | auto-spawn | Distance/SpatialHash/Scene/Match/Team; host visibility |

Reading the table cold, three consensus facts and one genuine fork:

1. **Everyone converges on per-connection set membership** maintained
   server-side, with a priority/importance mechanism kept strictly separate.
   Nobody who scaled kept the per-pair virtual call (Unreal built two
   successive systems to escape its own).
2. **Spatial scoping is a grid or a radius everywhere it ships.** PVS-grade
   occlusion culling exists only where a BSP already existed for rendering
   (Q3/Source, RING academically). Nobody builds visibility data *for*
   networking.
3. **The escape hatches are always the same four**: always-relevant,
   owner/single-connection, explicit set, and "everything" (the off switch).
   They appear in 1999 (`SVF_*`), 2004 (Source), 1998/2004 (Tribes/TNL),
   and every modern system, under different names.
4. **The fork is what the client does on exit** — destroy (Unreal, TNL,
   Unity, replicon, Mirror, lightyear) vs keep-and-dormant (Source; Unreal's
   dormancy gets there by a different door; placed-in-map actors are
   Unreal's partial concession). Destruction is simpler and bounds client
   memory; retention kills pop-in and preserves client-side state. Most of
   the industry chose destruction and then papered over the pop-in with
   grace periods (Unreal's 5 s) and hysteresis.

---

## 13. Ownership in Unreal — what the fused version actually does

The reference model, in its own documented words (all official 4.27 pages,
fetched; community marked).

- **Ownership is a chain, and the connection is found by walking it.** "Each
  connection has a PlayerController, created specifically for that
  connection… To determine if an actor in general is owned by a connection,
  you query for the actors most outer owner, and if the owner is a
  PlayerController, then that actor is also owned by the same connection that
  owns the PlayerController"
  ([Actors and their Owning Connections, official 4.27](https://dev.epicgames.com/documentation/en-us/unreal-engine/actors-and-their-owning-connections?application_version=4.27)).
  Pawns join the chain via possession and leave it at unpossession. The
  accessor is `GetNetConnection()` (5.3 page, snippet only).
- **What the chain gates, per the same official page**: RPC routing ("unless
  the RPC is marked as multicast, it needs to know which client to execute
  that RPC on"); relevancy ("For actors that have bOnlyRelevantToOwner set to
  true, only the connection that owns that actor will receive property
  updates"); per-connection conditions ("When COND_OnlyOwner is used, only
  the owners of that actor will receive these property updates"); and role
  assignment ("their role is downgraded to ROLE_SimulatedProxy while their
  properties are replicated to connections that don't own these actors").
  Four different jobs, one pointer.
- **Roles**: `ROLE_Authority` on the server ("only the server replicates
  actors to connected clients (clients will never replicate actors to the
  server)"); `ROLE_AutonomousProxy` — "generally only used on actors that are
  possessed by PlayerControllers. This just means that this actor is
  receiving inputs from a human controller, so when we extrapolate, we have
  a bit more information"; `ROLE_SimulatedProxy` — "the standard simulation
  path… extrapolating movement based on the last known velocity"
  ([Actor Role and RemoteRole, official 4.27](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-role-and-remoterole?application_version=4.27)).
  Autonomous-vs-simulated is *prediction targeting derived from ownership*:
  the same actor is autonomous on exactly the owning client.
- **The RPC drop table is ownership enforcement**: a client-invoked Server
  RPC on an actor owned by another client, by the server, or by nobody is
  "Dropped" — silently ([RPCs, official 4.27](https://dev.epicgames.com/documentation/en-us/unreal-engine/rpcs?application_version=4.27));
  a Client RPC on an actor with no owning connection: "this logic will not
  be executed" ([Networking Overview, official 4.27](https://dev.epicgames.com/documentation/en-us/unreal-engine/networking-overview?application_version=4.27)).
  The RPC survey (§1) recorded the same facts from the sending side; here
  note what they *are*: ownership acting as the security principal, with
  silence as the failure mode.
- **The condition vocabulary** ([Conditional Property Replication, official
  4.27](https://dev.epicgames.com/documentation/en-us/unreal-engine/conditional-property-replication?application_version=4.27),
  verbatim): `COND_OwnerOnly` "will only send to the actor's owner";
  `COND_SkipOwner` "send to every connection EXCEPT the owner" (the standard
  trick for not echoing predicted state back at the predictor);
  `COND_AutonomousOnly` / `COND_SimulatedOnly` (send by the *receiver's role
  for this actor*, i.e. by ownership again); `COND_InitialOnly`,
  `COND_InitialOrOwner`, `COND_SimulatedOrPhysics`, `COND_Custom`. This is
  per-field, per-connection-*class* relevancy, with the classes defined by
  ownership — the deepest point of fusion between this survey's two halves.
- **Transfer gotchas, documented by the people who debug them**: ownership
  changes are server-side (`SetOwner`), take effect for RPC routing on the
  server immediately, but a client "must wait for the server to replicate the
  updated… Owner reference" before its own owner-gated Server RPCs stop being
  dropped — the "client behaves as if the item was never picked up" symptom,
  with the stated fix "always call Item->SetOwner(PlayerCharacter). Do not
  rely on attachment to handle network routing"
  ([horizOn, community](https://horizon.pm/blog/multiplayer-inventory-nightmares-fixing-swapped-actorcomponent-owners-in-unreal-engine)).
  Note what the bug class *is*: five jobs keyed off one replicated pointer
  means every transfer is five simultaneous semantic changes, each with its
  own propagation delay.

## 14. Ownership decomposed — five jobs, and who separates which

Pull the word apart. "Ownership" in the systems surveyed is doing up to five
jobs:

1. **State authority** — who may write authoritative state for this entity.
2. **Input binding** — whose input stream drives this entity's simulation.
3. **Message routing** — where a directed message/RPC about this entity is
   delivered, and who may invoke one on it.
4. **Relevancy anchor** — whose view position scopes the world, and which
   entities are visible *only* to their controller.
5. **Prediction target** — which machine simulates this entity ahead of
   confirmation.

The evidence that these are separable is that shipping systems separate them:

- **Photon Fusion names 1 and 2 as different authorities.** "Every attached
  Network Object has an explicit or implied State Authority… Network
  Properties only replicate from the State Authority to other peers," while
  "The Input Authority indicates which Player's inputs should be returned
  for this Object when GetInput() is called" — and in server modes state
  authority is *pinned*: "the Server is ALWAYS the State Authority… State
  Authority cannot be changed," while input authority is assignable per
  object ([Network Object, official](https://doc.photonengine.com/fusion/current/manual/network-object);
  [Player, official](https://doc.photonengine.com/fusion/current/manual/playerref)).
  Prediction hangs off *input* authority, not state authority: inputs are
  consumed on the "InputAuthority: To simulate local movement immediately
  (Prediction)" and on the "StateAuthority: To process the authoritative
  move" ([player input, official](https://doc.photonengine.com/fusion/current/manual/data-transfer/player-input)).
  RPC permissions are then spelled *in terms of* the two authorities
  (`RpcSources.InputAuthority` etc. — RPC survey §7). In shared mode, state
  authority becomes per-object and transferable, but only pull-wise:
  "State Authority CANNOT be assigned… CAN be acquired" via
  `RequestStateAuthority()` gated by `AllowStateAuthorityOverride` or
  `ReleaseStateAuthority()` (Network Object page). Transfer designed as
  request/release, not imposition — the lesson from Unreal's transfer bugs,
  applied.
- **O3DE names the lattice.** Four per-entity roles: Authority ("full read
  and write access"); **Autonomous — "A role with the _illusion_ of write
  access… usually assigned to components directly under local user control…
  can also take advantage of predictive networking"**; Client ("strictly
  read-only"); Server (a *non-authoritative server's* view — "strictly
  read-only, and all interaction… should be handled using RPCs")
  ([O3DE multiplayer overview, official docs repo](https://raw.githubusercontent.com/o3de/o3de.org/main/content/docs/user-guide/networking/multiplayer/overview.md)).
  "Illusion of write access" is the most honest three-word description of
  client prediction in any documentation surveyed, and it makes jobs 2+5 a
  *role*, distinct from job 1.
- **lightyear splits 1, 4, and 5 into fields and keeps 2's anchor as its own
  component** (§8): `authority` (who simulates and sends), `target` (who
  receives), `sync` (who predicts vs interpolates), `controlled_by` (which
  client this belongs to, driving disconnect cleanup). Four axes, four
  fields, composable per entity.
- **Unity N4E splits 2 from 1 and derives 5 from a component**: `GhostOwner`
  "creates a bond/relationship in between an entity and a specific client"
  via a stored `NetworkId`; `GhostMode.OwnerPredicted` — "predicted for the
  client that owns it, and interpolated for all other clients"; the input
  binding is `CommandTarget`/`AutoCommandTarget`, whose documented conditions
  include "The ghost must be owned by your client (requiring the server to
  set the `GhostOwner` to your `NetworkId.Value`)"
  ([GhostOwner API](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/api/Unity.NetCode.GhostOwner.html);
  [command stream manual](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/command-stream.html)).
  Note the direction of derivation: ownership is *a component the server
  writes*; input routing and prediction *read* it. Authority never moves —
  the server writes all ghosts, full stop.
- **The fusers, and what fusion costs.** Unity NGO has one scalar
  (`OwnerClientId`) and re-grows the missing distinctions as flags — in
  distributed-authority mode ownership *becomes* state authority and needs a
  permission lattice bolted on (`Distributable` / `Transferable` /
  `RequestRequired` / `SessionOwner`, plus request callbacks and an
  ownership lock —
  [NGO ownership docs, official](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.11/manual/components/core/networkobject-ownership.html));
  its "prediction" is achieved by moving *write* authority to the owner
  (owner-authoritative `NetworkTransform`), i.e. simulating job 5 by
  reassigning job 1. Mirror is the sharpest cautionary quote in the set:
  "Client authority means that the client has control of an object" — but
  "the server still controls SyncVar and control other serialization
  features" ([Mirror authority docs](https://mirror-networking.gitbook.io/docs/manual/guides/authority))
  — so Mirror's "authority" transfers *Command rights and lifetime coupling*
  while state writing stays put; the word and the mechanism disagree inside
  one page. Godot compresses jobs 1-3 into a single per-node peer-id integer
  (`set_multiplayer_authority`, default peer 1, with the doc warning that a
  change "does **not** automatically replicate the new authority to other
  peers" — [Node class docs](https://docs.godotengine.org/en/stable/classes/class_node.html))
  and then routes *input* by escape hatch: the documented idiom for input is
  an `"any_peer"` RPC, i.e. input routing deliberately **outside** the
  authority model (high-level multiplayer tutorial).
- **The floor**: Source and Quake 3 have essentially no replicated ownership
  concept. Q3's is an index equation (`cl - svs.clients` binds a connection
  to its player entity; `clientNum` on entityState is rendering metadata —
  `sv_client.c`, `q_shared.h`). Source's is a *client-local prediction
  eligibility set*: the local player and its carried weapons
  (`SetPredictionEligible(true)` in `C_BasePlayer::OnDataChanged` for
  `IsLocalPlayer()` only; PredictableID matching in `CheckInitPredictable` —
  `c_baseplayer.cpp`, `c_baseentity.cpp`). Job 5 existing with no jobs 1-3
  at all — proof by construction that the jobs are independent.

**The verdict on the decomposition question.** Job 1 (state authority) is
architectural, not per-entity, in every server-authoritative system that has
not deliberately built peer authority: Unreal, Unity N4E, Mirror-in-practice,
replicon, Q3, Source, Overwatch. It becomes per-entity data only in shared/
distributed topologies (Fusion shared mode, NGO distributed authority,
lightyear, SpatialOS) — and then it needs request/release protocol, permission
flags, and receive-side filtering, an entire subsystem. Jobs 2-5 are
per-entity data everywhere, and they are *derivable from one field only in the
simplest case* — one player, one pawn, no spectators, no transfer. The
moment a game has spectate, possession swap, vehicles, or AI handoff, the
engines that derived everything from one pointer accumulate gotcha
documentation, and the engines that kept the jobs apart do not.

## 15. Ownership in ECS practice — component, relation, or nothing

The user's specific question: owner *component*, or implied through the
systems players write?

- **Every ECS surveyed that has the concept made it a component holding a
  connection/client id**: Unity's `GhostOwner { NetworkId }`, lightyear's
  `ControlledBy { owner, lifetime }` (server-side) with a `Controlled` marker
  (receiver-side). None derives ownership implicitly from which systems touch
  the entity — and the reason is structural, not stylistic: at least three
  *engine-side* consumers need to read it (input routing, directed-message
  recipients, disconnect cleanup), and an implicit convention inside game
  systems is invisible to the engine. bevy_replicon, which has no gameplay
  layer, simply has no entity-level concept at all — `AuthorizedClient` is
  connection-level — and leaves it to user components, which in practice
  means every replicon game reinvents `ControlledBy` (lightyear being the
  reinvention that got adopted).
- **Relations would express it, but nothing here requires them.** Flecs has
  first-class entity-to-entity relationships with an `Exclusive` trait —
  "ensures that an entity can have only a single instance of a relationship.
  When a second instance is added, it replaces the first instance"
  ([Flecs component traits](https://www.flecs.dev/flecs/md_docs_2ComponentTraits.html))
  — which is precisely `OwnedBy`-shaped: at most one target, replace on
  reassign, queryable from both ends ("all entities owned by X"). Bevy
  shipped one-to-many relationships in 0.16 (the `ChildOf`/`Children` pair,
  with many-to-many and value-fragmentation explicitly future work —
  [Bevy 0.16 notes](https://bevy.org/news/bevy-0-16/)) — correcting the
  common claim that Bevy has none — yet lightyear still models control as a
  plain component, because a component whose value is the owner *is already
  exclusive by construction*: one component slot, one value. The reverse
  index ("entities controlled by connection C") is the only thing relations
  add, and a map maintained by the replication layer covers it. Conclusion:
  **an owner component needs nothing fancier than a component**; relationship
  machinery is a nice-to-have for queryability, not a prerequisite.
- **What the component should *not* be called is "owner."** The two ECS
  namings that shipped chose words for the narrow job: `GhostOwner` (Unity —
  and its consumers say "owner-predicted", naming the derived behavior) and
  `ControlledBy` (lightyear — with `Lifetime` making the disconnect policy
  explicit). Fusion's vocabulary is the most precise of all: input authority
  vs state authority, never "owner." The survey's finding in one line: *the
  systems with the fewest ownership bugs are the ones whose names refuse to
  say "owner."*

---

## 16. Comparison table — ownership

| System | Job 1: state writes | Job 2: input binding | Job 3: message routing | Job 4: relevancy anchor | Job 5: prediction | Transfer | On disconnect |
|---|---|---|---|---|---|---|---|
| **Unreal** | server, always | possession → autonomous proxy | owner chain; unowned Server RPC **dropped** | view target + owner chain (`bOnlyRelevantToOwner`, `COND_OwnerOnly`) | derived: autonomous proxy on owning client | `SetOwner` server-side; documented client-visible races | PlayerController/Pawn torn down; unowned actors persist |
| **Fusion** | StateAuthority (server-pinned in server modes; per-object, request/release in shared) | **InputAuthority, separate & assignable** | `RpcSources`/`RpcTargets` in authority terms | AoI regions per player (separate system) | on InputAuthority | input auth assignable; state auth acquire-only | shared mode: objects orphan or despawn per config |
| **Unity N4E** | server, always | `CommandTarget`/`AutoCommandTarget` reads `GhostOwner` | RPCs target connection entities (not objects) | relevancy set is game-computed (may read GhostOwner) | `OwnerPredicted` reads `GhostOwner` | server rewrites `GhostOwner` | game-defined |
| **lightyear** | `AuthorityPeer`, transferable | via `ControlledBy` + input plugin | messages to `NetworkTarget` | rooms/relevance (separate) | `SyncTarget` per entity | `transfer_authority` | **`Lifetime`: SessionBased despawn / Persistent** |
| **NGO** | server (C/S); owner (distributed authority, permission flags) | fused into `OwnerClientId` | `RequireOwnership` on RPCs | `CheckObjectVisibility` (separate) | none real; owner-write instead | `ChangeOwnership` server-only; DA request flow | `DontDestroyWithOwner` flag |
| **Mirror** | server — even under "client authority" | player object per connection | `[Command]` requires authority | observer system (separate) | none built-in | `AssignClientAuthority` | client's objects destroyed by default |
| **Godot** | authority peer id (per node) | idiom: `"any_peer"` RPC — outside the model | `@rpc("authority")` | visibility filters (separate) | none | `set_multiplayer_authority`, not auto-replicated | game-defined |
| **O3DE** | Authority role | Autonomous role | RPCs between roles | per-role property subsets | Autonomous ("illusion of write access") | role reassignment | game-defined |
| **Source** | server | usercmd per connection | n/a | PVS from player eye; virtuals read owner for weapons/viewmodels | client-local predictable set | n/a | player entity torn down |
| **Quake 3** | server | usercmd → own player entity only | n/a | PVS from own eye; `SVF_SINGLECLIENT` | pmove over own playerState | n/a | client slot freed; entity removed |

---

## 17. What is actually standard, and what is one engine's idiosyncrasy

**Standard — safe to treat as settled:**

- Relevancy is **server-computed per-connection set membership**, evaluated
  before (and separately from) priority, with despawn as the client-visible
  consequence of leaving the set. Every scaled system converges here.
- The four escape classes: always-relevant, only-to-controller, explicit
  per-connection set, and off. Present from `SVF_*` to Iris filters.
- **Priority/importance as a starvation-proof accumulator** is separate from
  and downstream of relevancy. Tribes 1998 → Unreal `GetNetPriority` → Unity
  importance → Iris prioritizers → Assisi's existing accumulator.
- Spatial interest ships as **radius or grid**, never as networking-specific
  visibility precomputation.
- Ownership-as-input-binding is **a server-written component/field holding a
  connection id** in every ECS that has it.
- State authority in server-authoritative engines is **global, not
  per-entity** — an architecture fact, not entity data.

**Idiosyncratic — carrying one engine's history, not to be copied blindly:**

- **Unreal's owner *chain*** (relevancy and RPC rights resolved by walking
  `Owner` pointers to a PlayerController) — a consequence of actors owning
  actors; no ECS reproduces it, and its transfer races are its documented
  cost.
- **Unreal's destroy-on-irrelevant with a flat 5 s grace** — one point in a
  design space Source occupies differently (keep-and-dormant); neither is
  "the" answer, and the placed-in-map exception shows Epic itself hedging.
- **Dormancy as a separate mutable mode with flush discipline** — Unreal
  needed it because its change detection is per-actor polling; a system with
  acked per-entity baselines gets the bandwidth win structurally (§19).
- **PVS/PAS transmit culling** — free where a BSP pipeline already exists,
  unavailable everywhere else. The PAS *idea* (audio scopes wider than
  video) generalizes; the mechanism does not.
- **SpatialOS's general query engine** and **Donnybrook's attention model** —
  instructive extremes, unshipped or dead as products.
- **Fusion shared-mode transferable state authority / NGO distributed
  authority** — real systems, but each drags in a permission lattice and
  request protocol; they are what "ownership can move" costs when job 1 is
  per-entity.

---

## 18. What this suggests for Assisi

The recommendations, consistent with the RPC survey's §18 (whose recipient
classes presuppose exactly these mechanisms) and the opt-in plan's named
seam. Priority per the owner's direction: **base efficiency universal to all
games; information-boundary scoping stays a game-side option with engine
hooks, not an engine feature.**

### 18.1 Relevancy: a per-connection set filter at the named seam — and the lifecycle is already built

The single most important finding of this survey is local: **Assisi's
existing core already implements the hard half of the relevancy lifecycle.**
Every system above had to answer "what happens when an entity leaves/enters a
connection's set," and the answers cost them channels, grace timers, and
resync machinery. In Assisi, the server already computes despawns as *the set
difference between what the connection ackedly has and what is live*
(`Replication.hpp:279-288` — `Connection::acked` vs the live set), spawn and
late-join are already the empty-baseline path, and baseline entries are
already erased on acked despawn. Introduce a per-connection relevant set
`R(c)` and replace "live" with "live ∩ R(c)" in exactly one place — the
per-connection snapshot build, before priority accumulation, precisely where
the opt-in plan reserved the seam — and the whole lifecycle falls out:

- **Exit** ⇒ the entity leaves the connection's effective set ⇒ the existing
  despawn diff emits a despawn *that is resent until acked* — a reliable
  scope-exit, which is strictly stronger than Q3's implicit absence and
  equal to everyone else's explicit despawn.
- **Re-entry** ⇒ the NetId is absent from the acked set ⇒ the existing
  spawn/empty-baseline path resends full state — Unreal's "all properties
  sent on relevancy" behavior, already implemented and already
  budget-paginated.
- **Client side** ⇒ nothing new at all: a despawned mirror is a despawned
  mirror. The destroy-vs-dormant fork (§12 point 4) resolves to *destroy*
  for Assisi v1 — the client keeps no stale ghosts, `Mirrored` entities are
  session-disposable by design, and the pop-in cost is bounded by the same
  full-resend machinery late-join already exercises. If a game later wants
  Source-style retention, that is a client-side policy (keep the entity,
  strip the `Mirrored` liveness) layered on the same wire traffic — note it
  as a seam, build nothing.

Concretely, the mechanism recommendation:

1. **The engine owns the set, not the test.** Per connection, a bitset/hash
   of NetIds (`R(c)`), consulted by `SendSnapshot` and maintained by a
   *relevancy provider*. Providers are the pluggable part, Mirror-style.
   Ship two: **AllRelevant** (today's behavior, the default — at PIE/co-op
   scale, filtering is pure overhead, which is the performance-first answer)
   and **Distance** (radius around each connection's view anchor, the
   industry default from `NetCullDistanceSquared` to Fusion AoI). The
   provider interface — "given connection c and its view anchors, produce
   set changes" — is also *the* anti-cheat hook: a game that wants
   line-of-sight or fog-of-war scoping writes a provider; the engine
   guarantees a non-member entity contributes zero bytes to that connection
   (snapshots *and* the RPC design's all-relevant recipient class, which is
   computed from the same set — RPC survey §18). That guarantee is the hook;
   the LOS logic is the game's. Nothing further built engine-side.
2. **Evaluate on change where possible, per-snapshot where not.** The
   Distance provider re-tests per snapshot tick per connection over live
   replicated entities — at Assisi's scale (tens of connections, hundreds of
   replicated entities) that is thousands of squared-distance compares per
   snapshot, noise. The seam to leave for growth is the RepGraph lesson,
   recorded not built: if entity counts ever make the scan bind, the fix is
   a shared spatial grid whose cells hold entity lists, maintained on
   movement — membership evaluated where things *change*, not where they
   are read. No graph, no nodes, until a measured need.
3. **Hysteresis is mandatory in the Distance provider, from day one.** Enter
   radius strictly less than exit radius, plus a minimum-dwell (ticks before
   a revoke takes effect — Unreal's 5 s grace is the precedent, though its
   value should be far smaller here since re-entry is cheap). Boundary
   thrash otherwise converts every orbiting entity into a
   despawn/full-respawn cycle — the one failure mode every system either
   engineered around or suffered. (Honesty note: the literature pass found
   *no* citable academic treatment of boundary hysteresis — §19's
   could-not-verify — so this is engine lore, stated as such, with Unreal's
   timeout as the shipped example.)
4. **Escape classes, spelled as policy on `Replicated`**: always-relevant
   (the default when no provider filters — and the recommended *forever*
   default for anything plot-critical: the literature's power-law warning
   means a radius is a bandwidth tool, not a correctness tool);
   only-to-controller (reads §18.2's component; the owner-only class every
   system has); and per-connection explicit grants (the API form of
   `SVF_SINGLECLIENT` / `SetPlayerAlwaysInterested`, needed by spectator
   tooling and by any game-side provider). That is the full standard set
   from §17; resist inventing a fourth.
5. **What relevancy is *not*, in Assisi**: not dormancy (the acked-baseline
   core plus replicated sleep state already delivers idle-entity silence
   structurally — an idle entity costs zero bytes after ack *without any
   per-actor mode or flush discipline*, which is dormancy's entire job done
   better; do not build dormancy); not priority (the accumulator stays,
   downstream of the filter, exactly as Iris orders them); not the component
   gates (orthogonal, as the opt-in plan states). One sentence for the
   panel/docs: *gates decide what an entity says, relevancy decides who is
   listening, priority decides who is heard first.*
6. **Bookkeeping consequences to specify when built** (so they land as
   decisions): `IsWorldComplete` becomes "you have everything *relevant to
   you*" — correct as-is, worth a comment; the keyframe sweep re-anchors
   only the relevant set (it operates on baselines, so this is automatic);
   per-connection diagnostics gain a relevant-set size and an
   enter/exit-per-second counter — thrash must be visible in the Network
   panel, not inferred; and a body state must never be sent for an entity
   outside `R(c)` (the existing known-entity inclusion gate already implies
   it, but a test should pin it).

### 18.2 Ownership: one control component, and refuse the word "owner"

The user's question — component, or implied through systems — gets the
survey's unambiguous answer: **a component, because the engine has to read
it**; but a component that records *control*, deliberately not authority.

1. **Job 1 (state authority) stays architectural.** The server writes
   everything; clients send `InputCommand`s and nothing else (plan-v4 §3.1).
   Do not make authority per-entity data — that is the single biggest
   structural simplification Assisi gets to keep, and every system that made
   authority movable (Fusion shared, NGO distributed, lightyear) bought a
   permission lattice and a receive filter with it. The opt-in plan §6
   already records the tripwire: the day transferable authority lands, G2
   grows a server-side receive filter. Leave that sentence as the seam.
2. **One replicable component, suggested spelling `ControlledBy`** (the
   lightyear word; `GhostOwner` is the same thing with Unity's vocabulary):
   a connection/client id, written only by the server/session layer, absent
   on everything uncontrolled. It carries the jobs that are genuinely one
   concept — *this connection's player is this entity*:
   - **Input binding** (job 2): the session consumes a connection's
     `InputCommand`s against its controlled entities; gameplay systems query
     `(ControlledBy, …)` instead of guessing. This is `CommandTarget` /
     `InputAuthority` in Assisi's terms, and it is the first consumer the
     deferred pawn work (plan-v4 §5) needs anyway.
   - **Directed messages** (job 3): the RPC survey's *directed* recipient
     class ("to the owner") resolves through it; its *except-instigator*
     class resolves through the sender connection. No owner chain — the
     component is on the entity the message is about, full stop.
   - **Prediction flag** (job 5, later): when the pawn lands, "predict the
     entities you control" is a query, exactly Unity's `OwnerPredicted`
     derivation. Nothing to design now; the component is the anchor.
   - **Disconnect policy**: copy lightyear's `Lifetime` field verbatim in
     spirit — per-entity `despawnOnDisconnect` (default true for
     player-spawned things, false for world objects a player merely
     controls). Disconnect handling then has one home: despawn or strip
     `ControlledBy`, both through existing paths.
   Exclusivity needs no relationship machinery: one component, one value
   (§15). A reverse map (connection → controlled entities) lives in the
   session layer beside the NetId maps, maintained at the same
   reconcile point.
3. **Job 4 (the relevancy anchor) deliberately does *not* read
   `ControlledBy`.** Make the anchor per-connection session state — one or
   more view positions/entities the game sets, *defaulting* to the
   connection's controlled entities when present. Reasons, all from the
   survey: v1's joiner is a spectator with no controlled entity at all
   (plan-v4 §5) yet still needs a view anchor for any Distance provider;
   Unreal's anchor is actually the view target, not the pawn, and spectate/
   camera actors are where owner-derived anchoring leaks; and TNL's "current
   control object" phrasing already implies the indirection. One small
   decoupling now prevents re-fusing the jobs this section exists to keep
   apart. The *only-to-controller* relevancy class does read the component —
   that is its job description.
4. **AI and world props: uncontrolled is the default and needs nothing.**
   No `ControlledBy`, always server-driven, relevancy by provider like
   everything else. The engines confirm by silence — nobody has an "owner"
   for a crate; Unreal's unowned actors simply have no owning connection and
   lose only the owner-gated features. An AI temporarily "possessing" a
   vehicle is a gameplay fact, not a networking one, until a *connection*
   controls it — at which point it is a `ControlledBy` write, which is also
   the whole transfer story: **transfer = the server rewriting one
   component**, replicated like any other component, with none of Unreal's
   five-simultaneous-changes race because the other four jobs read it
   through queries on arrival rather than caching a chain walk. The one
   real transfer race that remains — a client invoking a directed verb
   before learning it controls the entity — is structurally absent in v1
   (clients send only `InputCommand`s) and becomes a documented
   server-side-tolerance rule ("input for an entity you don't control is
   dropped, counted, not an error") when the pawn lands.

### 18.3 What not to build, with reasons

- **No PVS or occlusion-based transmit culling.** BSP-era machinery; Assisi
  has no precomputed visibility structure and should not grow one for
  networking (§17). The hook (game-side provider) covers the game that
  wants it.
- **No replication graph.** Fortnite's numbers (100 × 50,000) are three
  orders of magnitude past the target scale; the recorded grid seam is the
  insurance.
- **No dormancy mechanism.** §18.1 point 5 — the acked baseline already is
  the better version.
- **No per-entity state authority / no transferable authority.** §18.2
  point 1, with the opt-in plan's tripwire left in place.
- **No engine-side line-of-sight/fog-of-war scoping.** Per the owner's
  direction: efficiency is universal, information boundaries are per-game.
  The engine's whole obligation is the zero-bytes guarantee for non-members
  plus the provider/anchor/explicit-grant hooks — all three of which §18.1
  ships for efficiency reasons anyway.
- **No aura-pair or attention-model matching.** O(n²) pairwise interest and
  Donnybrook-style attention scoring are the literature's dead ends for
  this scale (§11); the priority accumulator already delivers the useful
  fraction (graceful degradation by authored importance).

### 18.4 The one-paragraph version

Relevancy: a per-connection NetId set, filtering the snapshot build at the
seam the opt-in plan already named; providers pluggable, AllRelevant default,
Distance-with-hysteresis shipped, spatial grid recorded as the growth path;
exit and re-entry ride the existing acked-set despawn diff and empty-baseline
resend, so the lifecycle costs nothing new; despawn-on-exit on the client.
Ownership: keep authority architectural (server writes everything), add one
server-written `ControlledBy{connection, despawnOnDisconnect}` component
carrying input binding, directed-message routing, the future prediction flag,
and disconnect policy; keep the relevancy anchor as per-connection session
state that merely defaults to controlled entities; transfer is a component
write. Both halves are one predicate and one component away from the code
that exists, and both were designed — per the ground rule — for the engine
Assisi is growing into, not the one it is today.

---

## 19. What I could not verify

- **Unreal, globally**: no Unreal claim in this survey comes from engine
  source; all are official doc pages (fetched, cited), Epic-staff
  statements, or community write-ups, marked in place. Specifically
  unverified: the modern `UNetDriver::RelevantTimeout` property's own doc
  text and 5.0 default (the *behavior* — "If not relevant for 5 seconds,
  close channel" — is official 4.27; the property name/default rest on an
  unfetchable API page and a legacy-era community wiki snippet);
  `ROLE_None`'s official description and the exact `GetLocalRole` /
  `GetRemoteRole` accessor docs; any official end-to-end statement that
  re-relevancy is destroy → new channel → full resend (assembled from three
  verified partial statements); official per-node descriptions for the
  RepGraph node classes (community only); whether a GDC 2018 talk dedicated
  to the Replication Graph exists under that name (the 100-player/50k-actor
  numbers are in the official doc, so nothing rests on it); and "SetOwner is
  server-only" as an enforced restriction rather than required practice.
- **Overwatch's relevancy**: no source found stating whether Overwatch
  performs per-client entity culling at all; the role-based payload split is
  verified from the Reed deck, and "no spatial culling at 6v6" is inference,
  marked in §10. The Ford-talk authority quotes circulating ("some clients
  are jerks") are secondary transcriptions and were not used as
  load-bearing.
- **Source engine internals**: the engine-side snapshot pipeline (PVS/PAS
  set construction, the "leavepvs" packed-entity flag) is closed source;
  everything Source-side is verified at the game-DLL boundary
  (source-sdk-2013) or from Wayback captures of the wiki, as cited. The
  literal comment "force the transmission of the owner" was *not* found —
  the real comment is "Force our aiment and move parent to be sent."
- **Boundary hysteresis in the literature**: asserted throughout engine
  folklore, but no fetched academic source discusses region-boundary
  thrash/hysteresis explicitly. §18's hysteresis recommendation is
  engineering practice with Unreal's grace timeout as the only shipped,
  citable analogue.
- **Liu & Theodoropoulos (ACM CSUR 2014)** full text — paywalled; taxonomy
  verified from the author's 2012 thesis instead. **Boulanger's NetGames
  2006 paper** PDF — host down; claims rest on the (fetched) thesis
  superset. **IEEE 1516 / DIS spec text** — paywalled; Petty 2002 and
  Morse 1996 used as primaries about them.
- **EVE's sub-solar-system "grid" mechanism** — community-documented only;
  no first-party source found. **WoW's cell/grid AoI** — no first-party
  source; only survey assertions and emulator reverse-engineering, so it is
  cited only as "zone-based" on Liu's authority.
- **SpatialOS QBI's cost history** — no first-party postmortem found; only
  the official docs' own caveats (quoted). All SpatialOS claims are from
  archive.org captures, marked as such.
- **Unity N4E**: an explicit "relevancy costs CPU" warning was looked for
  and not found; the per-tick set-rebuild model is documented, its cost is
  arithmetic. **lightyear**: the book lags the 0.28 crates; every
  load-bearing claim is pinned to a version-specific docs.rs page as cited;
  `PredictionTarget` could not be located in the 0.28 item index (the
  receive/predict split is verified at 0.17.1).
- **Fusion**: the literal inspector field names for the object-interest
  modes (e.g. an `AoiMode` field) — the manual documents the modes, not the
  serialized field names. `AssignInputAuthority`'s exact permission rule
  beyond "Only valid when called on a Host or Server peer"
  (RemoveInputAuthority's verified phrasing).
- **Godot**: lazy parent-inheritance of multiplayer authority — what is
  verified is the `recursive = true` push-down default and the
  not-auto-replicated warning; treat "inherited unless overridden" as
  unconfirmed.
- **OpenTNL**: the exact `ScopeAlways` symbol (the fetched pages document
  the per-connection always-scope methods and a `ScopeLocal` flag; the
  class-level flag name was not confirmed). The Tribes-paper quote
  "transmits only the objects that are relevant" that circulates is **not
  verbatim** — the accurate scope quotes are in §3.

## Sources

- **In-repo**: docs/replication-plan-v4.md · docs/replication-optin-plan-v1.md
  · docs/replication-research-ecs-survey.md ·
  docs/replication-research-rpc-survey.md · `modules/NetSync/include/Assisi/
  NetSync/Replication.hpp`, `NetComponents.hpp`
- **Quake 3** (id-Software/Quake-III-Arena, master): `code/server/
  sv_snapshot.c` · `code/server/sv_world.c` · `code/server/sv_client.c` ·
  `code/client/cl_parse.c` · `code/cgame/cg_snapshot.c` · `code/game/
  g_public.h` · `code/qcommon/q_shared.h` · Sanglard:
  <https://fabiensanglard.net/quake3/network.php>
- **Source** (ValveSoftware/source-sdk-2013, master, `src/`):
  `game/server/gameinterface.cpp` · `game/server/baseentity.cpp` ·
  `game/server/basecombatweapon.cpp` · `game/server/basecombatcharacter.cpp`
  · `game/server/baseviewmodel.cpp` · `game/server/ServerNetworkProperty.cpp`
  · `game/server/recipientfilter.{h,cpp}` ·
  `game/shared/SoundEmitterSystem.cpp` · `game/client/c_baseentity.cpp` ·
  `game/client/c_baseplayer.cpp` · `public/edict.h` ·
  `public/iservernetworkable.h` · Valve wiki via Wayback 2024:
  Networking_Entities, Source_Multiplayer_Networking, PVS
  (<https://developer.valvesoftware.com/wiki/Networking_Entities> etc.) ·
  SignOn states: alliedmodders/hl2sdk `csgo` branch `common/protocol.h`
- **Tribes/TNL**: <https://www.gamedevs.org/uploads/tribes-networking-model.pdf>
  · <http://opentnl.sourceforge.net/doxydocs/classTNL_1_1GhostConnection.html>
  · <http://opentnl.sourceforge.net/doxydocs/classTNL_1_1NetObject.html>
- **Unreal, official (fetched)**: Actor Relevancy and Priority (4.27):
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-relevancy-and-priority?application_version=4.27>
  · Detailed Actor Replication Flow (4.27):
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/detailed-actor-replication-flow?application_version=4.27>
  · Actor Network Dormancy:
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-network-dormancy-in-unreal-engine>
  · Replication Graph (4.27):
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/replication-graph?application_version=4.27>
  · Introduction to Iris / Iris Filtering / Iris Prioritization:
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-filtering-in-unreal-engine>
  · Actors and their Owning Connections (4.27):
  <https://dev.epicgames.com/documentation/en-us/unreal-engine/actors-and-their-owning-connections?application_version=4.27>
  · Actor Role and RemoteRole (4.27) · RPCs (4.27) · Conditional Property
  Replication (4.27) · Networking Overview (4.27) · Python API class Actor
  (4.27, property tooltips)
- **Unreal, community**: Neukirchen:
  <https://cedric-neukirchen.net/docs/multiplayer-compendium/actor-relevancy-and-priority/>
  and <https://cedric-neukirchen.net/docs/multiplayer-compendium/ownership/> ·
  WizardCell: <https://wizardcell.com/unreal/multiplayer-tips-and-tricks/> ·
  vorixo: <https://vorixo.github.io/devtricks/initial-dormancy/> and
  <https://vorixo.github.io/devtricks/iris-replication-filter/> · hzFishy:
  <https://notes.hzfishy.fr/Unreal-Engine/Networking/Core/Dormancy-and-relevancy>
  · Kieran Newland:
  <https://www.kierannewland.co.uk/replication-graph-how-to-reduce-network-bandwidth-in-unreal/>
  · UE forums (relevancy deletion):
  <https://forums.unrealengine.com/t/stopping-actor-deletion-when-outside-of-network-cull-distance-on-clients/2097087>
  · horizOn (ownership transfer):
  <https://horizon.pm/blog/multiplayer-inventory-nightmares-fixing-swapped-actorcomponent-owners-in-unreal-engine>
  · Matt Gibson (adaptive frequency):
  <https://www.mattgibson.dev/blog/unreal-replication-settings>
- **Unity Netcode for Entities**: optimizations:
  <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/optimizations.html>
  · GhostRelevancy API:
  <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/api/Unity.NetCode.GhostRelevancy.html>
  · GhostOwner API:
  <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/api/Unity.NetCode.GhostOwner.html>
  · GhostMode API · GhostSendType API · command stream:
  <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/command-stream.html>
  · ghost snapshots:
  <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/ghost-snapshots.html>
- **bevy_replicon**: VisibilityPolicy (0.32):
  <https://docs.rs/bevy_replicon/0.32.0/bevy_replicon/server/enum.VisibilityPolicy.html>
  · ClientVisibility (0.32 and latest) · visibility source:
  <https://github.com/simgine/bevy_replicon/blob/master/src/shared/replication/visibility.rs>
  and `src/server/visibility.rs` · AppVisibilityExt:
  <https://docs.rs/bevy_replicon/latest/bevy_replicon/server/visibility/trait.AppVisibilityExt.html>
  · CHANGELOG:
  <https://github.com/simgine/bevy_replicon/blob/master/CHANGELOG.md> ·
  AuthorizedClient · ClientEntityMap · ServerEntityMap (docs.rs, as cited)
- **lightyear**: book, interest management:
  <https://cbournhonesque.github.io/lightyear/book/concepts/advanced_replication/interest_management.html>
  · book, authority:
  <https://cbournhonesque.github.io/lightyear/book/concepts/advanced_replication/authority.html>
  · book, client replication · Replicate (0.17.1):
  <https://docs.rs/lightyear/0.17.1/lightyear/prelude/server/struct.Replicate.html>
  · ControlledBy / Lifetime (0.28):
  <https://docs.rs/lightyear_replication/latest/lightyear_replication/control/struct.ControlledBy.html>
  · visibility::immediate (0.28):
  <https://docs.rs/lightyear_replication/latest/lightyear_replication/visibility/immediate/index.html>
- **SpatialOS (archived)**: authority-and-interest:
  <https://web.archive.org/web/20210928045444/https://documentation.improbable.io/spatialos-overview/docs/authority-and-interest>
  · write-access-authority:
  <https://web.archive.org/web/20210928043108/https://documentation.improbable.io/spatialos-overview/docs/write-access-authority>
  · query-based-interest:
  <https://web.archive.org/web/20210504162630/https://documentation.improbable.io/spatialos-overview/docs/query-based-interest>
  · set-up-query-based-interest:
  <https://web.archive.org/web/20210928044255/https://documentation.improbable.io/spatialos-overview/docs/set-up-query-based-interest>
  · GDK interest:
  <https://web.archive.org/web/20210928050011/https://documentation.improbable.io/gdk-for-unreal/docs/game-client-interest-management>
- **Photon Fusion**: Network Object:
  <https://doc.photonengine.com/fusion/current/manual/network-object> ·
  PlayerRef: <https://doc.photonengine.com/fusion/current/manual/playerref> ·
  player input:
  <https://doc.photonengine.com/fusion/current/manual/data-transfer/player-input>
  · interest management:
  <https://doc.photonengine.com/fusion/current/manual/advanced/interest-management>
  · NetworkObject API:
  <https://doc-api.photonengine.com/en/fusion/current/class_fusion_1_1_network_object.html>
- **Unity NGO**: ownership:
  <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.11/manual/components/core/networkobject-ownership.html>
  · ownership concepts (2.10) · distributed authority (2.10) · object
  visibility:
  <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.6/manual/basics/object-visibility.html>
  · NetworkTransform (2.7)
- **Mirror**: authority:
  <https://mirror-networking.gitbook.io/docs/manual/guides/authority> ·
  interest management:
  <https://mirror-networking.gitbook.io/docs/manual/interest-management> ·
  InterestManagement.cs:
  <https://github.com/MirrorNetworking/Mirror/blob/master/Assets/Mirror/Core/InterestManagement.cs>
- **Godot**: Node (multiplayer authority):
  <https://docs.godotengine.org/en/stable/classes/class_node.html> ·
  MultiplayerSynchronizer:
  <https://docs.godotengine.org/en/stable/classes/class_multiplayersynchronizer.html>
  · high-level multiplayer:
  <https://docs.godotengine.org/en/stable/tutorials/networking/high_level_multiplayer.html>
- **O3DE**: multiplayer overview (docs source):
  <https://raw.githubusercontent.com/o3de/o3de.org/main/content/docs/user-guide/networking/multiplayer/overview.md>
- **Flecs / Bevy**: relationships:
  <https://www.flecs.dev/flecs/md_docs_2Relationships.html> · component
  traits (Exclusive):
  <https://www.flecs.dev/flecs/md_docs_2ComponentTraits.html> · Bevy 0.16
  release notes: <https://bevy.org/news/bevy-0-16/>
- **Literature**: Benford & Fahlén 1993:
  <https://www.lri.fr/~mbl/ENS/CSCW/2013/papers/Benford_CSCW1993.pdf> ·
  MASSIVE: <https://people.cs.nott.ac.uk/pszcmg/massive1/massive.html> ·
  Boulanger thesis: <https://www.cs.mcgill.ca/~jboula2/thesis.pdf> ·
  Boulanger et al. NetGames 2006 (abstract):
  <https://dl.acm.org/doi/10.1145/1230040.1230069> · Petty 2002 (DDM):
  <https://www.uah.edu/images/research/cmsa/pdf/Pubs_Dr_Petty/Petty%202002%20Comparing%20DDM%201.3%20%201516.pdf>
  · Morse 1996: <https://escholarship.org/uc/item/9n9895jx> · Funkhouser,
  RING 1995: <https://www.cs.princeton.edu/~funk/symp95.pdf> · Bharambe et
  al., Donnybrook 2008:
  <https://pages.cs.wisc.edu/~akella/CS838/F09/838-Papers/p389-bharambe.pdf>
  · Liu thesis 2012:
  <http://etheses.bham.ac.uk/id/eprint/3710/1/Liu12PhD.pdf> · Liu &
  Theodoropoulos CSUR 2014 (abstract):
  <https://dl.acm.org/doi/10.1145/2535417> · Yahyavi & Kemme CSUR 2013
  (mirror):
  <https://romisatriawahono.net/lecture/rm/survey/network%20security/Yahyavi%20-%20Architectures%20for%20Massively%20Multiplayer%20Online%20Games%20-%202013.pdf>
- **MMO practice**: CCP single-shard (Game Developer):
  <https://www.gamedeveloper.com/game-platforms/feature-ccp-outlines-single-shard-mmo-development>
  · High Scalability, EVE architecture:
  <https://highscalability.com/eve-online-architecture/> · Second Life
  server architecture: <https://wiki.secondlife.com/wiki/Server_architecture>
