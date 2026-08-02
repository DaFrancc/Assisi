# How ECS engines handle replication — a survey

*Research note, 2026-08-02. Compiled to inform where Assisi's replication opt-in
should live (see docs/replication-plan-v4.md for the shipped v1 design). Every
implementation claim carries its source inline; anything inferred or unverifiable
is marked as such.*

**Lineage note (useful framing):** most of the vocabulary here — *ghost*,
*scoping*, *priority*, *state mask* — comes from the 1998 Tribes networking
model, which already had per-object priority-ordered ghosting and server-side
scoping to decide relevance ([The TRIBES Engine Networking Model, Frohnmayer &
Gift](https://www.gamedevs.org/uploads/tribes-networking-model.pdf)). Unity
NetCode is a fairly direct descendant. Where systems disagree today is mostly
*where the declaration lives*, not what gets sent.

---

## Unity NetCode for Entities (DOTS)

- **Entity opt-in:** a prefab with a `GhostAuthoringComponent`. That component
  carries `Name`, `Importance`, `Supported Ghost Mode`, `Default Ghost Mode`,
  `Optimization Mode`
  ([ghost-snapshots](https://docs.unity3d.com/Packages/com.unity.netcode@1.3/manual/ghost-snapshots.html)).
  Non-ghost entities are simply never considered.
- **Granularity / polarity:** **opt-in, per-field, declared globally on the
  component type.** `[GhostField]` on public fields; "Once a component has at
  least one field marked with `[GhostField]`, it becomes replicated." Private
  members are ignored; buffers require *all* public fields marked (same page).
- **Capability vs policy:** yes, and it is the most developed version of this
  split found. Capability is the type-level `[GhostField]` + `[GhostComponent]`
  (`PrefabType`, `SendTypeOptimization`, `SendToOwnerType`,
  `SendDataForChildEntity`). Policy is **per-ghost-prefab, per-component
  override** via the `GhostAuthoringInspectionComponent` — "It's possible to
  define the desired behavior in code, and on a per-ghost prefab basis, and on a
  per-component basis"
  ([ghost-types-templates](https://docs.unity3d.com/Packages/com.unity.netcode@1.3/manual/ghost-types-templates.html)).
  Third layer: `[GhostComponentVariation]` swaps the generated serializer for a
  type you don't own; `[DontSerializeVariant]`, `[ClientOnlyVariant]`,
  `[ServerOnlyVariant]` are prebuilt policies.
- **Change detection:** server serialization runs **per ECS chunk, not per
  entity**, then does a value comparison: netcode "serializes the data and
  compares it with the previous synchronized version," with delta-compression
  against a per-connection baseline. Documented gotcha: granting a job *write
  access* to a `GhostField` component costs CPU even if nothing changes
  ([optimizations](https://docs.unity3d.com/Packages/com.unity.netcode@1.3/manual/optimizations.html)).
  `Optimization Mode = Static` means the ghost isn't sent at all while unchanged.
- **Relevancy:** two independent mechanisms. `GhostRelevancy` singleton with
  `GhostRelevancyMode` = `Disabled` / `SetIsRelevant` / `SetIsIrrelevant`, plus a
  `GhostRelevancySet` of (connection, ghost) pairs and a `GlobalRelevantQuery` —
  i.e. **explicit relevancy sets**, with opt-in *or* opt-out polarity selectable
  at runtime. Separately, `Importance` / `GhostImportance`
  (`ScaleImportanceFunction`, `GhostDistanceImportance` + `GhostDistanceData`
  tiles) is a *bandwidth prioritizer*, not a filter (same page).
- **Cost when not networking:** `GhostComponentAttribute.PrefabType` "allows you
  to remove the component from the specific version of the ghost prefab" —
  components are **stripped at bake time** from the server or client variant
  (e.g. rendering components off the server prefab). For a genuinely
  non-networked context, netcode supports a `LocalSimulation` world — "a world
  that does not run any Netcode systems"
  ([client-server-worlds](https://docs.unity3d.com/Packages/com.unity.netcode@1.3/manual/client-server-worlds.html)).
  *Inference (not documented):* `[GhostField]` on a type used only by non-ghost
  entities costs compile-time codegen and binary size, but no runtime work.

---

## bevy_replicon

- **Entity opt-in:** the `Replicated` marker component. "By default no entities
  are replicated. Add the `Replicated` marker component on the server for
  entities you want to replicate"
  ([docs.rs](https://docs.rs/bevy_replicon/latest/bevy_replicon/)). Removing it
  now stops replication *without* despawning on clients
  ([CHANGELOG 0.41.0](https://github.com/simgine/bevy_replicon/blob/master/CHANGELOG.md)).
  Clients get `Remote`, not `Replicated`, since 0.40.
- **Granularity / polarity:** **opt-in, per component type, registered
  globally** via `AppRuleExt`: `replicate::<C>()`, `replicate_as()`,
  `replicate_bundle()`, `replicate_filtered()`, `replicate_group::<(A, B)>()`,
  `replicate_once()`, `replicate_diff()`. Registered components replicate on
  *every* entity carrying `Replicated`. The closest thing to per-entity
  component policy is archetype-shaped rules — `*_filtered` methods take
  `With`/`Without`/`Or` (CHANGELOG 0.35.0), so "replicate `Transform` only on
  entities that also have `Player`" is expressible as a rule.
- **Capability vs policy:** capability = the registered rule; entity policy =
  `Replicated`; **per-client, per-component policy = the visibility system**.
  `VisibilityFilter` is a "Component that controls remote entity visibility,"
  with `VisibilityScope` = `All` / `Components` / `AllExcept` ("Hides every
  component on the entity except those in `S` when the filter denies
  visibility")
  ([visibility module](https://docs.rs/bevy_replicon/latest/bevy_replicon/shared/replication/visibility/index.html)).
  This is genuinely per-client-per-component, described as working "similarly to
  layers in physics."
- **Change detection:** Bevy's own change ticks — "We use Bevy's change
  detection to track and send changes." Bevy's `is_changed()` fires on **mutable
  dereference, not value inequality**
  ([bevy_ecs DetectChanges](https://docs.rs/bevy_ecs/latest/bevy_ecs/change_detection/trait.DetectChanges.html)),
  so replicon inherits false positives that Unity's value-compare pass would
  filter. Clients track a last-applied tick and unacked mutations are resent.
- **Relevancy:** the visibility filters above; `ClientVisibility` remains as a
  low-level bitset layer. Bandwidth shaping via `PriorityMap` (per-client) and
  `ReplicatePriority` (per-entity default) (CHANGELOG 0.35/0.41). No built-in
  spatial grid.
- **Cost when not networking:** explicitly designed for it — "For singleplayer
  replication systems won't run at all and for listen server replication will
  only be sending" (docs.rs). Supports "singleplayer, client, dedicated server,
  and listen server configurations simultaneously"
  ([README](https://github.com/simgine/bevy_replicon)).

---

## lightyear

**Important:** as of **0.27.0 lightyear "Switched the replication backend to
`bevy_replicon`"**
([releases](https://github.com/cBournhonesque/lightyear/releases)). The two Bevy
answers have converged at the transport/diffing layer; lightyear is now the
higher-level prediction/interpolation/authority layer on top.

- **Entity opt-in:** the `Replicate` component — "Insert this component to start
  replicating your entity"
  ([prelude](https://docs.rs/lightyear/latest/lightyear/prelude/index.html)),
  typically `Replicate::to_clients(NetworkTarget::All)`
  ([vladbat00, community blog, Aug 2025, lightyear 0.23](https://vladbat00.github.io/blog/000-spawning-entities/)).
  Hierarchy is opt-out via `DisableReplicateHierarchy` / `ReplicateLike`.
- **Granularity / polarity:** opt-in, per component type:
  `app.component::<C>().replicate().predict()` (0.27 unified flow). Older
  `register_component_once` maps to `ReplicationMode::Once`;
  `ComponentReplicationConfig::replicate_once` is the per-type knob for markers
  that never change.
- **Capability vs policy:** capability = registration; policy = per-entity
  `Replicate` + `NetworkTarget`, plus **runtime authority transfer**
  (`HasAuthority`, `AuthorityBroker`, `GiveAuthority`, `RequestAuthority`) —
  lightyear is the only Bevy option treating authority as first-class movable
  state.
- **Relevancy:** two modes in `lightyear_replication::visibility` — `immediate`
  ("immediately update the network visibility of an entity for a given client")
  and `room` ("semi-static rooms"), with `RoomAllocator` allocating global
  `RoomId`s and `Rooms` inserted on both entities and clients
  ([module docs](https://docs.rs/lightyear_replication/latest/lightyear_replication/visibility/index.html),
  releases 0.27).
- **Notable:** lightyear also ships `DeterministicPredicted` /
  `lightyear_deterministic_replication` — input-only lockstep with
  snapshot-based late-join catch-up (releases 0.22, 0.27). Same library, both
  replication philosophies.

---

## Flecs

- **Nothing built in.** The FAQ has no networking/multiplayer/replication entry
  at all
  ([FAQ.md](https://raw.githubusercontent.com/SanderMertens/flecs/master/docs/FAQ.md));
  no official roadmap statement found either way — **couldn't verify** whether
  it is deliberately out of scope.
- **Building blocks it does give you:** "Flecs has builtin change detection.
  Additionally, you can use an `OnSet` observer to get notified of changes to
  component values" (FAQ). Change detection is **table/archetype-level, opt-in
  per query** (`.detect_changes()` when building the query), implemented as
  per-component dirty counters on tracked tables plus a counter for add/remove;
  `it.skip()` preserves the changed state
  ([Queries.md](https://github.com/SanderMertens/flecs/blob/master/docs/Queries.md)).
  Note the granularity mismatch: you learn *a table changed*, not *which
  entity*.
- Serialization comes from the meta/reflection addon; the Remote API is
  JSON/REST and Flecs positions it beyond tooling ("can be used in production
  environments to use Flecs as a backend datastore that can be dynamically
  queried" — [Flecs Remote API](https://www.flecs.dev/flecs/md_docs_2FlecsRemoteApi.html)),
  but it is not a game replication protocol.
- **Real-world data point:** Hytale used Flecs and said so publicly
  ([Hytale ECS technical explainer, 2024](https://hytale.com/news/2024/6/summer-2024-technical-explainer-hytale-s-entity-component-system-opwpcamdi))
  — but that post contains **zero** networking content, and the only replication
  descriptions are unofficial community reverse-engineering
  ([doctale.dev](https://doctale.dev/plugin-development/networking/client-sync/),
  undated, unattributed). Treat Hytale replication as **couldn't verify**.

---

## EnTT

- **No built-in networking**, by design. What exists is serialization:
  `entt::basic_snapshot` — "a snapshot can be either a dump of the entire
  registry or a narrower selection of components of interest… the latter is
  suitable for creating client-server applications" — and
  `entt::basic_continuous_loader`, which "load[s] data from a source registry to
  a (possibly) non-empty destination," mapping remote entity identifiers to
  local ones
  ([entity crash course](https://skypjack.github.io/entt/md_docs_2md_2entity.html),
  [basic_continuous_loader](https://skypjack.github.io/entt/classentt_1_1basic__continuous__loader.html)).
- **Community pattern**, per the author himself: there is "no one-catch-all
  solution"; use "a bitset on your `NetSync`, update it on change (maybe using
  `on_update`?)", or "mark your components as *dirty* when changed", or a custom
  pool that "keep[s] track of them for you". He explicitly warns against
  replicating everything changed: "there are data that should not be sent to a
  server because they are useful only for the local simulation"
  ([discussion #617](https://github.com/skypjack/entt/discussions/617)). Change
  tracking is now `entt::reactive_mixin`/reactive storage; the old `observer`
  class was deprecated in v3.14
  ([v3.14 changelog discussion](https://github.com/skypjack/entt/discussions/1133)).
- So in EnTT the opt-in decision is **entirely yours** — a marker component + a
  per-type registry table is the de facto pattern.

---

## Unreal — classic Actor replication

- **Entity opt-in:** `bReplicates` on the actor instance/class. **Property
  opt-in:** `UPROPERTY(Replicated)` + `GetLifetimeReplicatedProps` +
  `DOREPLIFETIME`. This is **per class, not per instance** — every instance of
  the class replicates the same property set.
- **Per-audience policy** is the `DOREPLIFETIME_CONDITION` / `COND_*` enum
  (`COND_OwnerOnly`, `COND_SkipOwner`, `COND_InitialOnly`, `COND_Custom`,
  `COND_ServerOnly`) — Unreal's capability/policy split, and it is
  *per-property-per-connection-class*, not per-instance.
- **Change detection:** by default, **poll and compare against a shadow copy of
  last-sent state** every net update. Push model inverts it:
  `FDoRepLifetimeParams::bIsPushBased`, `net.IsPushModelEnabled=1`, and
  `MARK_PROPERTY_DIRTY_FROM_NAME` /
  [`UNetPushModelHelpers::MarkPropertyDirty`](https://docs.unrealengine.com/5.1/en-US/API/Runtime/Engine/Net/UNetPushModelHelpers/MarkPropertyDirty/)
  at the mutation site — "Properties marked with `WithPushModel` are **assumed
  clean** unless explicitly dirtied"
  ([StraySpark, community blog](https://www.strayspark.studio/blog/iris-push-model-ue5-7-replication-frameworks)).
- **Relevancy:** ordered checks in `AActor::IsNetRelevantFor()` —
  `bAlwaysRelevant` / owned by the pawn or PC, then `bNetUseOwnerRelevancy`,
  then `bOnlyRelevantToOwner`, attachment inheritance, hidden+non-colliding,
  then distance vs `NetCullDistanceSquared`. Bandwidth via `NetPriority` ×
  time-since-last-replicated in `AActor::GetNetPriority()`, plus
  `NetUpdateFrequency`
  ([Actor Relevancy and Priority](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-relevancy-and-priority-in-unreal-engine);
  detailed writeup:
  [Cedric Neukirchen's compendium](https://cedric-neukirchen.net/docs/multiplayer-compendium/actor-relevancy-and-priority/),
  community).
- **Dormancy** is the separate "stop considering this at all" axis:
  `ENetDormancy` = `DORM_Never` / `DORM_Awake` / `DORM_DormantAll` /
  `DORM_DormantPartial` / `DORM_Initial`, with `FlushNetDormancy` to force one
  more update
  ([Actor Network Dormancy](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-network-dormancy-in-unreal-engine);
  `DORM_Initial` nuances:
  [vorixo, community blog](https://vorixo.github.io/devtricks/initial-dormancy/)).

## Unreal — Iris

- **Same declarations, different backend.** Iris is "an opt-in replication
  system that works alongside Unreal Engine's existing replication system";
  existing replicated properties and RPCs "are compatible with minor
  modification." Enabled via the Iris plugin, `bUseIris = true`,
  `net.Iris.UseIrisReplication=1`, and requires
  `net.SubObjects.DefaultUseSubObjectReplicationList=1`
  ([Introduction to Iris](https://dev.epicgames.com/documentation/unreal-engine/introduction-to-iris-in-unreal-engine)).
- **Change detection / architecture:** Iris "keep[s] a full copy of all
  replicated state data in a quantized form" and performs expensive work once,
  sharing it across connections. Objects are identified by `FNetRefHandle`;
  dirtiness is "a bitfield tracking the dirtiness of members" inside replication
  states; sending is pre-send (copy/quantize dirty data, reset dirtiness) then
  schedule/serialize by priority
  ([Components of Iris](https://dev.epicgames.com/documentation/en-us/unreal-engine/components-of-iris-in-unreal-engine)).
  Non-`AActor` objects must implement `RegisterReplicationFragments`, usually
  via `FReplicationFragmentUtil::CreateAndRegisterFragmentsForObject` with
  `Params.bIsPushBased = true`
  ([BorMor, community blog](https://bormor.dev/posts/iris-uobject-replication/)).
- **Relevancy is restructured into a filter stack:** owner filter
  (`UE::Net::ToOwnerFilterHandle`), connection filter, group filter
  (`CreateGroup()` → `FNetObjectGroupHandle`, `AddGroupFilter`,
  `SetGroupFilterStatus(group, connectionId, ENetFilterStatus)`, `AddToGroup`),
  and dynamic filters deriving from `UNetObjectFilter` — shipped:
  `UFilterOutNetObjectFilter`, `UNetObjectConnectionFilter`,
  `UNetObjectGridFilter`, registered in
  `[/Script/IrisCore.NetObjectFilterDefinitions]`. Applied with
  `ReplicationSystem->SetFilter(ObjectNetHandle, FilterHandle)`. Documented
  constraint: dynamic filters "cannot enable replication for an object that has
  already been filtered out by connection or group filters"
  ([Iris Filtering](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-filtering-in-unreal-engine)).
- **Prioritization is separate from filtering:** `SetStaticPriority(handle,
  1.0f)` or `SetPrioritizer` with
  `GetPrioritizerHandle(FName("SphereNetObjectPrioritizer"))`; built-ins are
  `SphereNetObjectPrioritizer`, `SphereWithOwnerBoostNetObjectPrioritizer`,
  `NetObjectCountLimiter`. Priority ≥ 1.0 makes an object eligible this frame;
  lower priorities accumulate
  ([Iris Prioritization](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-prioritization-in-unreal-engine)).
  Iris explicitly replaces Replication Graph's node model with filters +
  prioritizers.

## Unreal — Mass Entity replication

- **Opt-in is a trait on the entity config asset** — `UMassReplicationTrait` —
  which names a `UMassReplicatorBase` subclass and an `AMassClientBubbleInfoBase`
  subclass; the framework then adds `FMassReplicationSharedFragment` and
  `FMassNetworkIDFragment`
  ([MassReplication API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/MassReplication);
  mechanism writeup:
  [Ignacio de la Vega, community blog](https://blog.ignaciodelavega.com/Unreal-Engine/Unreal-Engine-Mass-replication)).
- **Granularity is not per-fragment.** You hand-write
  `ProcessClientReplication()` in a `UMassReplicatorBase` subclass to copy
  fragment data into an `FReplicatedAgentBase` struct, which rides a fast-array
  in a per-client "bubble" (`FMassClientBubbleSerializerBase`, one
  `AMassClientBubbleInfo` actor per client). Clients apply it back via
  `PostReplicatedAdd` / `PostReplicatedChange`. So Mass punts entirely on
  declarative component replication — **replication is imperative code, not a
  declaration.**
- **Relevancy is LOD-driven:** `FMassReplicationLODFragment`; you add LOD tag
  requirements to the replicator so irrelevant entities aren't replicated.
  `UMassReplicationProcessor` runs server-only.

---

## Photon Quantum — the deliberate opposite

- **There is no entity/component replication system at all.** Clients "only
  exchange player input with the simulation running locally on all clients"
  ([Quantum intro](https://doc.photonengine.com/quantum/current/quantum-intro)).
  Components are declared in `.qtn` DSL with **zero networking annotations** —
  `component Action { FP Cooldown; FP Power; }`
  ([DSL](https://doc.photonengine.com/quantum/current/manual/quantum-ecs/dsl)).
  The capability/policy/granularity questions dissolve rather than get answered.
- The only axis is *in the Frame or not*: anything in the frame is shared by
  construction; non-shared state must live in the Unity view layer.
- **Change detection:** none. Verified vs predicted frames; "Quantum always
  rolls-back and re-simulates frames"
  ([Frames](https://doc.photonengine.com/quantum/current/manual/frames)).
- **Full state exists only as bootstrap/resync:** snapshots are "a platform
  independent blob… the complete state of the game after a verified tick," used
  for late-join and reconnect
  ([Reconnecting](https://doc.photonengine.com/quantum/current/manual/game-session/reconnecting)).
- **Relevancy is structurally impossible** and Photon says so: "client-controlled
  secrets used in a card game and Fog Of War-like features are easily hackable"
  ([Cheat protection](https://doc.photonengine.com/quantum/current/manual/cheat-protection)).
  Area-of-Interest exists in Photon *Fusion*, not Quantum
  ([Fusion interest management](https://doc.photonengine.com/fusion/current/manual/advanced/interest-management)).
- **Cost when not networking:** offline is first-class
  (`DeterministicGameMode.Local`), but the determinism tax — `FP` fixed-point
  math, blittable aligned frame layout, manual alloc/free of frame collections —
  is paid unconditionally. Quantum 3 didn't change any of this
  ([What's new](https://doc.photonengine.com/quantum/current/getting-started/whats-new)).

---

## Overwatch

Primary source is Dan Reed's **GDC 2017** deck, whose full slides are public:
[Networking Scripted Weapons and Abilities in Overwatch](https://media.gdcvault.com/gdc2017/Presentations/Reed_Dan_NetworkingScriptedWeapons.pdf).
Tim Ford's architecture talk is members-only video with no slide PDF — anything
attributed to Ford is secondary. There is no "NetworkStateComponent" in any
findable source — **couldn't verify**; the real names are `StatescriptComponent`,
`StatescriptSyncMgr`.

- **Three-layer opt-in, the most directly transferable idea here.** (1)
  **Compile-time type capability, default off:** the `SYNC_ALL` attribute —
  "With attribute: StatescriptStates transmit locally and remotely… Without
  attribute: StatescriptStates only transmit locally; StatescriptActions do not
  transmit." (2) **Runtime instance policy:** "Runtime objects may opt out of
  transmitting." (3) **Per-audience payload computed offline by a compiler
  pass** that determines which nodes and variables remote clients need, baked
  into `m_remoteSyncNodes` / `m_syncVars` with precomputed bit widths.
- **Field granularity** is an explicit `mirror` block in the `.stu` schema
  naming exactly which runtime fields participate, read/written by
  `WriteMirroredData()` / `ReadMirroredData()`. Deltas go finer still — down to
  affected array index ranges.
- **Change detection:** dirty sets recorded *during* simulation, never a diff
  against a shadow copy. `StatescriptDeltas` per command frame; per-client
  `StatescriptGhost` holds the last-acked frame; a packet is built by taking "a
  union of all StatescriptDeltas in the Command Frame range" and serializing
  **current** values. Frame range starting at 0 = full-state bootstrap.
  Acknowledged cost: "The eventual-consistency networking model does not provide
  a perfect blow-by-blow replication."
- **Relevancy is role-based, not spatial:** local entity packets "must contain
  everything"; remote entities get only what remote instances reference.
  Measured: 2028 bits local vs 806 bits remote for a Tracer clip+reload. **No
  spatial/PVS relevancy found — couldn't verify** (plausibly unnecessary at
  6v6; that is inference).
- **Cost when not networking:** Unsynchronized Instances — same VM, zero sync
  cost — used for "Menus, Hero Gallery, End-of-match flow, Music," versus
  synchronized for "Weapons, Abilities, Emotes, Game modes, Map Entities."

---

## SpatialOS (archival, but the cleanest capability/policy split found)

All canonical doc domains are DNS-dead; the following is reconstructed from live
source in `github.com/spatialos` (archived Jan 2022), which is stronger evidence
anyway.

- **Capability is tri-state**, gating codegen: `SPATIALCLASS_SpatialType` /
  `SPATIALCLASS_NotSpatialType` / neither, checked in
  [`SpatialGDKEditorSchemaGenerator.cpp`](https://github.com/spatialos/UnrealGDK/blob/release/SpatialGDK/Source/SpatialGDKEditor/Private/SchemaGenerator/SpatialGDKEditorSchemaGenerator.cpp).
  The distinction between "explicitly not networked" and "nobody has said yet"
  is what makes a codegen diagnostic possible instead of silent non-replication.
  They *moved from opt-out to opt-in* because blanket schema generation didn't
  scale
  ([CHANGELOG](https://github.com/spatialos/UnrealGDK/blob/release/CHANGELOG.md)).
- **Policy is per-entity, per-component:** `improbable.EntityAcl` maps component
  ID → `WorkerRequirementSet` for write authority.
- **The `COND_*` buckets become separate wire components** — see the checked-in
  fixture with `SpatialTypeActor` and `SpatialTypeActorServerOnly` as distinct
  components
  ([SpatialTypeActor.schema](https://github.com/spatialos/UnrealGDK/blob/release/SpatialGDK/Source/SpatialGDKTests/SpatialGDKEditor/SpatialGDKEditorSchemaGenerator/ExpectedSchema/SpatialTypeActor.schema)).
  A subscriber can take the public bucket without ever receiving the server-only
  one.
- **Interest is declarative and composable:** `QueryConstraint` supports
  sphere/cylinder/box, *relative* variants anchored to the querier,
  `EntityIdConstraint`, `ComponentConstraint` (interest by component presence),
  and And/Or composition
  ([Interest.h](https://github.com/spatialos/UnrealGDK/blob/release/SpatialGDK/Source/SpatialGDK/Public/Schema/Interest.h)).
  Crucially the **result type is a component subset**, so "position only at
  200 m, full state at 20 m" is directly expressible — with per-query max
  frequency, merge-on-coalesce, and highest-frequency-wins when queries overlap.
- **ECS-native change detection (Unity GDK), three tiers:**
  `.WithAll<HasAuthority>()` (authority as an **archetype tag**, so
  non-authoritative entities cost nothing), then `.WithChangeFilter<Component>()`
  (chunk version), then an explicit `IsDataDirty()`/`MarkDataClean()` bit to
  kill false positives from no-op writes
  ([codegen template](https://github.com/spatialos/gdk-for-unity/blob/master/workers/unity/Packages/io.improbable.gdk.core/.codegen/Source/Generators/Core/UnityComponentReplicationSystemGenerator.cs)).
  No shadow copies anywhere.
- **Cost when off:** whole-stack only — a `bSpatialNetworking` project setting
  falls back to native Unreal networking entirely.

---

## Godot MultiplayerSynchronizer (non-ECS contrast)

- **Opt-in is per-instance and authored:** a `MultiplayerSynchronizer` node + a
  `SceneReplicationConfig` resource listing property `NodePath`s
  ([class ref](https://docs.godotengine.org/en/stable/classes/class_multiplayersynchronizer.html),
  [SceneReplicationConfig](https://docs.godotengine.org/en/stable/classes/class_scenereplicationconfig.html)).
  Existence replication is a separate node, `MultiplayerSpawner`.
- **Capability and policy are fused** — there is no type-level "this class can
  replicate" anywhere. Sharing a saved `SceneReplicationConfig` across instances
  is convention only (*inferred from `Resource` semantics*).
- Per property: `spawn` (default `true`) and `mode` (default
  `REPLICATION_MODE_ALWAYS`). Non-obvious coupling: **ALWAYS = unreliable,
  ON_CHANGE = reliable** — the mode picks transport reliability, not just
  cadence.
- **Change detection is reflection + deep-copied shadow + `hash_compare`** in
  `MultiplayerSynchronizer::_watch_changes`
  ([source](https://github.com/godotengine/godot/blob/master/modules/multiplayer/multiplayer_synchronizer.cpp));
  changed set is a `uint64` index mask, which caps a synchronizer at 64
  on-change properties (*inferred from the mask width*).
- **Relevancy:** per-peer booleans — `public_visibility` defaults `true`
  (opt-*out*, inverse polarity to property selection),
  `set_visibility_for(peer, bool)`, `add_visibility_filter(Callable)`.
  Visibility gates *spawning*, not just syncing. No spatial/distance built-in.
- **Cost when off is structurally zero**, and this is the best idea in the
  system: `_watch_changes()` is reachable only from `_send_delta()`, which lives
  inside `for (KeyValue<int, PeerInfo> &E : peers_info)`
  ([scene_replication_interface.cpp](https://github.com/godotengine/godot/blob/master/modules/multiplayer/scene_replication_interface.cpp)).
  Zero peers ⇒ the loop body never runs ⇒ no polling, no shadow copies. No
  feature flag, nothing to remember to disable.

---

## Minecraft Bedrock (thin, honestly marked)

- Bedrock uses EnTT — but only third-party confirmed, via EnTT's own README
  listing Minecraft as a user ([entt README](https://github.com/skypjack/entt)).
  **No first-party description of Bedrock's runtime ECS exists** — couldn't
  verify. Do not confuse this with the JSON `minecraft:health`-style "entity
  components" in
  [Microsoft's creator docs](https://learn.microsoft.com/en-us/minecraft/creator/reference/content/entityreference/examples/entitycomponents/minecraftcomponent_health),
  which are content authoring and document no networking semantics.
- **The wire protocol is officially published and is emphatically not
  ECS-shaped:** a hand-authored packet catalogue (`AddActorPacket`,
  `SetActorDataPacket`, `MoveActorAbsolutePacket`, …) with no generic
  "component N of entity E changed" message
  ([Mojang/bedrock-protocol-docs](https://github.com/Mojang/bedrock-protocol-docs)).
  `SetActorDataPacket` carries a `SynchedActorDataList` of `DataItemEntry` — an
  **integer-keyed tagged-union metadata bag**, plus newer index-addressed
  `PropertySyncData`
  ([SetActorDataPacket.html](https://github.com/Mojang/bedrock-protocol-docs/blob/main/docs/SetActorDataPacket.html)).
  Strong evidence the ECS was a simulation choice deliberately not propagated to
  the network layer.
- Opt-in polarity, capability/policy, and interest management: **couldn't
  verify** (closed source).
- A Microsoft-sponsored UW capstone explored an ECS replication layer with
  per-component-type declaration, but it is student work, not a Bedrock spec
  ([PDF](https://www.ece.uw.edu/wp-content/uploads/2021/05/Microsoft_ApplyTheDesignPatternOfEntityComponentSystemECSForACross-platformNetworkLayer_RoeeHorowitz_ErikHuang_RogerLiao.pdf)).

---

## Comparison table

| System | Entity opt-in mechanism | Component granularity | Default polarity | Relevancy approach |
|---|---|---|---|---|
| **Unity NetCode** | `GhostAuthoringComponent` on a prefab | Per-**field** `[GhostField]`, type-global; per-prefab/per-component override via inspection component + variants | Opt-in | Explicit relevancy sets (`GhostRelevancySet`, mode-switchable in/out) + `Importance` prioritizer w/ distance tiles |
| **bevy_replicon** | `Replicated` marker | Per component type (`replicate::<C>()`); archetype-filtered rules; per-client-per-component **visibility scopes** | Opt-in | Component-based visibility filters (`All`/`Components`/`AllExcept`) + priority maps; no spatial built-in |
| **lightyear** | `Replicate` component (+ `NetworkTarget`) | Per component type; `ReplicationMode::Once`; replicon backend since 0.27 | Opt-in | Immediate per-client visibility **or** rooms; also has an input-only deterministic mode |
| **Flecs** | — none — | — | — | — (change detection is table-level, opt-in per query) |
| **EnTT** | — none — (`NetSync`-style marker is convention) | Snapshot selects component types explicitly | — | — |
| **Unreal classic** | `bReplicates` per actor | Per **class** property list (`DOREPLIFETIME`) + `COND_*` per-connection-class | Opt-in | `IsNetRelevantFor` chain + `NetCullDistanceSquared` + `NetPriority`; dormancy as a separate "stop considering" axis |
| **Unreal Iris** | Same declarations; Iris is a backend swap | Same, but state is quantized once and shared across connections; dirtiness is a member bitfield | Opt-in | Filter stack: owner / connection / group / dynamic (`UNetObjectGridFilter`), plus separate prioritizers |
| **Unreal Mass** | `UMassReplicationTrait` on the entity config | **Not declarative** — hand-written `ProcessClientReplication()` into a client bubble | Opt-in | LOD-driven (`FMassReplicationLODFragment`, LOD tag requirements) |
| **Photon Quantum** | None — every entity is on every client | None; "in the Frame" is the only bit | Mandatory, no opt-out | Impossible by construction; documented as a fog-of-war/hidden-info limitation |
| **Overwatch** | Networked-entity category; sync gated on `StatescriptComponent` | Instance / node-type (`SYNC_ALL`) / declared `mirror` field / array index range | Opt-in, default off | Role-based (local vs remote), compiler-computed; no spatial AOI found |
| **SpatialOS** | Exists in the runtime; `SpatialType` UCLASS gate + `bReplicates` | Schema component; `COND_*` buckets become **separate wire components** | Opt-in (tri-state); tightened from opt-out | Declarative interest queries: geometry ∪ component-presence, **component-subset results**, per-query Hz |
| **Godot** (non-ECS) | `MultiplayerSynchronizer` node instance | Explicit list of property `NodePath`s, per instance | Opt-in (empty config) | Per-peer boolean + user `Callable` filters; gates spawn too; no spatial built-in |
| **Bedrock** | couldn't verify | Integer-keyed metadata bag (not typed components) | couldn't verify | couldn't verify |

---

## Where the genuine disagreements are

**1. Whether "replicable" is a property of the type or of the instance — and
whether that split even exists.** Unity NetCode, Overwatch, and SpatialOS all
deliberately separate compile-time capability from runtime policy (Overwatch's
`SYNC_ALL` + "runtime objects may opt out" is the sharpest statement of it), and
SpatialOS goes further by making capability *tri-state* so "explicitly not
networked" is distinguishable from "undeclared" and can be diagnosed at codegen
time. Godot fuses the two into a single per-instance authored resource with no
type-level concept at all. Unreal splits differently again: capability is per
*class* (the `DOREPLIFETIME` list is immutable per class), policy is per
*instance* (`bReplicates`) and per *connection class* (`COND_*`) — but never per
instance-per-property.

**2. Change detection is the loudest disagreement, and it's a real
correctness/cost trade, not a preference.** Three incompatible answers: (a)
**compare against a shadow copy** — Unreal classic, Godot; correct but
O(properties × actors) every tick and needs deep copies; (b) **trust ECS write
barriers** — bevy_replicon on Bevy change ticks, which fire on `DerefMut` and
therefore **send on no-op writes**; (c) **record dirty sets during simulation,
read values at send time** — Overwatch, which never diffs at all. Unity NetCode
splits the difference: chunk-level filtering *plus* a value comparison, and its
own docs warn that merely granting write access to a `GhostField` costs CPU.
SpatialOS's Unity GDK is the only one that layers all three (archetype tag →
chunk version → explicit dirty bit) precisely because tiers (b) and (c) each
have known failure modes alone.

**3. Whether relevancy filtering is even permitted.** Quantum's determinism buys
bandwidth proportional to player count rather than entity count, and pays for it
by making fog-of-war and hidden information "easily hackable" — Photon documents
this as a limitation, not a bug. Everyone else filters, but disagrees on the
shape: explicit relevancy sets (Unity), component-layer filters (replicon),
rooms (lightyear), a filter *stack* with documented precedence where dynamic
filters cannot re-enable what a group filter denied (Iris), and declarative
composable queries whose **result is a component subset rather than a whole
entity** (SpatialOS — the only system where "position only at long range, full
state up close" is a first-class expression rather than a hand-rolled variant).

**4. Whether the not-networking cost is designed away or merely toggled.**
bevy_replicon documents that singleplayer runs no replication systems at all;
Godot achieves the same result *structurally* — change detection is physically
nested inside the per-peer send loop, so zero peers means zero work, with no
flag to forget; Unity strips components from the wrong-side prefab at bake time
and offers a `LocalSimulation` world. SpatialOS, by contrast, only offers a
whole-stack `bSpatialNetworking` off-switch — you cannot make it incrementally
cheap. Quantum is the extreme case: local mode is free, but the determinism tax
(fixed-point math, blittable frames, manual frame-collection lifetimes) is paid
in single-player exactly as in multiplayer.

**5. A live convergence worth knowing:** the Bevy ecosystem's two "different
choices" are now one choice at the bottom — lightyear 0.27 switched its
replication backend to bevy_replicon, keeping only prediction, interpolation,
authority transfer, and rooms as its own layer.
