# How engines handle RPCs and networked events — a survey

*Research note, 2026-08-02. The events companion to
docs/replication-research-ecs-survey.md, which covered state. Written to inform
the RPC design deferred in docs/replication-plan-v4.md §5 ("still their own
future design; validation is security surface, not a rider"). Every
implementation claim carries its source inline; anything inferred or
unverifiable is marked as such. Where this note and the state survey would
overlap, this one defers — read them together.*

---

## 0. What is being asked, and why the ECS question is interesting

An RPC is the *other* half of a replication system. State replication answers
"what is true"; an RPC answers "what just happened" — a one-shot that has no
current value to converge on. The two are different because their failure modes
are different: a lost state update is repaired by the next one, and a lost event
is simply gone.

The intuition being tested is that RPC design "doesn't vary much between OOP and
ECS engines, since it's all based on functions." The interesting part is the
premise. In an OOP engine an RPC is *a method on an object*, and the object is
both the address and the security principal — Unreal's whole model falls out of
that (`AActor` ownership decides who may call, the actor channel decides who
receives). An ECS has no object to call a method on. Something has to fill that
role, and what each ECS chose turns out to be a live disagreement with real
consequences for versioning, validation, and ordering.

Assisi's position going in: state travels as delta snapshots against
per-connection acked baselines; `NetTransport` already offers
`SendMode::Reliable`/`Unreliable` across three lanes (`Control`, `Snapshot`,
`Bulk` — `NetTransport.hpp:50-76`); a protocol hash covering component layout
and framing gates the handshake (`NetProtocol.hpp:85-98`); reflectgen parses
`ACOMP`/`AFIELD` **structs and fields**, not functions; and the plan of record
carries a **state-first rule** — nothing that has a current value becomes an
event.

---

## 1. Unreal — classic RPCs (the thing to compare against)

- **Authoring surface:** direction and reliability are `UFUNCTION` specifiers —
  `Server`, `Client`, or `NetMulticast`, plus `Reliable`/`Unreliable`, with the
  body written in a `_Implementation` suffixed function. The specifiers are the
  whole declaration; UHT generates the marshalling
  ([Cedric Neukirchen's compendium, community](https://cedric-neukirchen.net/docs/multiplayer-compendium/remote-procedure-calls/);
  [RPCs, UE 4.27 docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/rpcs?application_version=4.27)).
- **Addressing:** the RPC is a method on a replicated `AActor` or a replicated
  subobject — "They must be called on Actors or a replicated Subobject (e.g. a
  component)" and "The Actor (and component) must be replicated" (compendium).
  The object *is* the address.
- **Direction and routing:** routing is derived from the actor's **owning
  connection**, not chosen per call. A `Client` RPC executes only on the client
  that owns the actor; a `Server` RPC requires that "the client must own the
  Actor that the RPC is being called on," and a client-invoked Server RPC on an
  unowned actor is **"Dropped"** (compendium). `NetMulticast` reaches every
  client the actor is *relevant to*, which is the relevancy system deciding
  recipients rather than the caller
  ([WizardCell, community](https://wizardcell.com/unreal/multiplayer-tips-and-tricks/)).
- **Reliability and ordering:** reliable RPCs are ordered *within one actor and
  its subobjects* — "The order of RPC execution on the receiving machine is
  respected for all RPCs called on an actor and its subobjects" — and explicitly
  not across actors: "There is no mechanism in which the original call order of
  RPCs across multiple actors is preserved and reapplied on a remote machine."
  Nor between reliability classes: "The order of RPC execution between unreliable
  and reliable RPCs can seem preserved, but this is never guaranteed"
  ([Replicated Object Execution Order](https://dev.epicgames.com/documentation/en-us/unreal-engine/replicated-object-execution-order-in-unreal-engine)).
- **Ordering *versus state* is specified, and it is the wrong way round for
  events about state:** the same page lists "RPCs are executed first," then
  "Properties are updated second," with "Property updates … sent as a single,
  unreliable data block," and unreliable multicasts as the exception, running
  *after* property updates. So a reliable multicast announcing a state change
  can execute **before** the property carrying that state arrives.
- **Reliable is a footgun with a documented cliff.** The reliable buffer is
  `RELIABLE_BUFFER`, 256 by default; `UChannel::SendBunch` drops the connection
  when outstanding unacked reliable bunches exceed it, via
  `Connection->SendCloseReason(ENetCloseResult::ReliableBufferOverflow)` then
  `Connection->Close(...)`
  ([vorixo, community](https://vorixo.github.io/devtricks/data-stream/)). Hence
  the standing advice: "Don't mark every RPC as Reliable!" and "Calling reliable
  RPCs on Tick can have side effects, such as filling the reliable buffer, which
  can cause other properties and RPCs to not be processed" (compendium). The
  mitigation is manual budgeting — "our budget to be half the size of the
  reliable buffer (`RELIABLE_BUFFER / 2`)" (vorixo). **A general-purpose
  reliable RPC channel whose overuse disconnects players is a design that
  offloads flow control onto every call site.**
- **Multicast is additionally throttled:** "A Multicast function will not
  replicate more than twice in a given Actor's network update period"
  (compendium).
- **Validation:** `WithValidation` plus a `_Validate` sibling returning `bool`;
  returning false "disconnect[s] the caller," and "if the validation function for
  an RPC detects that any of the parameters are bad, it can notify the system to
  disconnect the client" (compendium). **It is optional** — "Validation is
  optional for ServerRPC functions"
  ([Cyrex, community](https://cyrex.tech/rpc-validation-with-unreal-engine/)),
  which also frames the exposure: "this procedure call, straight from the
  player, is where hackers and malicious actors could work most effectively."
  There is separately an engine-level RPC flood defence (`FRPCDoSDetection`,
  `ERPCDoSEscalateReason` — [UE API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/ERPCDoSEscalateReason)),
  but I did not verify its default configuration — see §14.
- **State vs events:** the community's rule is unanimous and the docs' own
  design supports it — "Use replicated properties for replicating stateful
  events. Use Multicast/Client RPCs for replicating transient (not stateful) or
  cosmetic in nature events" (WizardCell). The failure it prevents:
  "New connections have no clue which RPCs were sent recently," so a mesh
  changed by multicast means "The newly spawned client won't see the new mesh"
  ([vorixo, stateful events](https://vorixo.github.io/devtricks/stateful-events-multiplayer/)).
- **Versioning:** **couldn't verify** a documented per-RPC compatibility
  mechanism. Unreal gates whole connections on engine/network version rather
  than on an RPC-set hash, as far as I could confirm.
- **Batching and cost:** **couldn't verify** a documented per-call overhead or
  batching policy for the legacy path.

## 2. Unreal — Iris, and what it did *not* change

- **The model is untouched; the scheduler is new.** Epic's own framing is that
  "RPC declaration and execution in Iris works as it does in the generic
  replication system and replication graph," pointing at the same Replication
  Execution Order page (surfaced via search of
  [dev.epicgames.com Iris docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine);
  the Iris pages render client-side and resisted direct fetch — see §14).
- **What actually changed is the queueing.** RPCs become `FNetRPC` net blobs
  created by `UNetRPCHandler`, managed by `FNetBlobManager`, enqueued through
  `FNetObjectAttachmentSendQueue`; reliability and ordering are flags evaluated
  at enqueue time — `CreationInfo.Flags` gains `ENetBlobFlags::Reliable` from
  `FUNC_NetReliable`, and gains `ENetBlobFlags::Ordered` for any RPC that is not
  `FUNC_NetMulticast`
  ([UE forums thread](https://forums.unrealengine.com/t/iris-unreliable-rpcs-such-as-clientack-are-being-treated-as-reliable/2609151)).
- **Unreliable unicast RPCs are put in the reliable queue on purpose**, and Epic
  confirmed it: "It is intended for the unreliable RPCs to end up in the reliable
  queue, as this is done to make sure these RPCs are executed in the expected
  order," while "They should not be resent if they are dropped" — reliability
  and ordering deliberately decoupled, with unicast ordering chosen to mimic
  legacy behaviour where "unicast RPCs are written to the bunch as soon as they
  are called" (Alex Koumandarakis, Epic, same thread).
- **Verdict on Iris for this question:** it is a state-replication rewrite
  (quantize once, share across connections, filter stack, prioritizers — see the
  state survey's Iris entry). For RPCs it changed the transport scheduling and
  left the authoring model, the ownership routing, and the validation story
  exactly where they were. **Iris is not the answer to "has anyone fixed
  RPCs".**

## 3. Unity Netcode for GameObjects — the same shape, cleaner spelling

- **Authoring:** "Declare an RPC by marking a method with the `[Rpc]` attribute
  and including the `Rpc` suffix in the method name"; an IL post-processing pass
  rewrites the call sites
  ([rpc.html, NGO 2.5](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/advanced-topics/message-system/rpc.html)).
- **Addressing:** an instance method — the method must be "declared in a class
  that inherits from NetworkBehaviour" on a GameObject with a `NetworkObject`
  ([messaging-system.html, NGO 2.7](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.7/manual/advanced-topics/messaging-system.html)).
- **Direction:** NGO's one genuine improvement over Unreal's spelling —
  direction became **a parameter, not a separate attribute**. `SendTo.Server`,
  `NotServer`, `Owner`, `NotOwner`, `Me`, `NotMe`, `Everyone`, `ClientsAndHost`,
  `SpecifiedInParams`, plus `AllowTargetOverride` for runtime targeting
  (rpc.html). "As of Netcode for GameObjects version 1.8.0, the `Rpc` attribute
  encompasses server to client RPCs, client to server RPCs, and client to client
  RPCs"
  ([rpc.html, NGO 1.14](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@1.14/manual/advanced-topics/message-system/rpc.html));
  `ServerRpcAttribute` is deprecated "in favor of using RpcAttribute with a
  Server target"
  ([API](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.7/api/Unity.Netcode.ServerRpcAttribute.html)).
  Runtime recipients via `RpcTarget.Single()` / `Group()` / `Not()`
  ([rpc-params.html](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/advanced-topics/message-system/rpc-params.html)).
- **Reliability and ordering:** reliable by default, `Delivery =
  RpcDelivery.Unreliable` to opt out; "In-order reliable RPC execution is
  guaranteed per NetworkObject basis only"; RPCs sent with no connection are
  "dropped/ignored"
  ([reliability.html](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/advanced-topics/message-system/reliability.html)).
  Same per-object ordering island as Unreal.
- **A detail worth stealing:** `DeferLocal` / `LocalDeferMode.Defer` — "If true,
  RPCs that execute locally will be deferred until the start of the next frame,
  as if they had been sent over the network" (rpc.html). This makes the
  host/listen-server path behave like the remote path instead of silently
  running a frame early — a whole class of "works when hosting, breaks on a
  client" bugs closed by one flag.
- **Validation:** `RequireOwnership` — "If true, this RPC throws an exception if
  invoked by a player that does not own the object" (rpc.html). Ownership gate
  only; no validation hook comparable to `_Validate`.
- **State vs events, stated as a rule with a test question:** "Sending state
  with RPCs won't be transmitted to late joining clients"; the heuristic is
  "Should a player joining mid-game get that information?"; "Use RPCs for
  transient events, information only useful for a moment when it's received. Use
  `NetworkVariable`s for persistent states"
  ([rpcvnetvar.html](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/learn/rpcvnetvar.html)).
- **Versioning:** `NetworkConfig.ProtocolVersion` ("Different versions doesn't
  talk to each other") and `CompareConfig(ulong hash)` comparing a SHA256 of the
  config
  ([API](https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.2/api/Unity.Netcode.NetworkConfig.html)).
  Per-method mismatch behaviour: **couldn't verify**.

## 4. Unity Netcode for Entities — the ECS answer, in full

This is the crux of the question, so it gets the most detail.

- **Authoring:** an RPC is **a component struct**, not a method: "extend
  `IRpcCommand`," and serialization is code-generated for simple structs. The
  manual path — needed for arrays and unusual payloads — is
  `IRpcCommandSerializer<T>` with `Serialize`, `Deserialize`, and
  `CompileExecute` producing a Burst `FunctionPointer`, with the docs warning to
  "make sure the `Serialize` and `Deserialize` calls are symmetric"
  ([rpcs.html, com.unity.netcode 1.10](https://docs.unity3d.com/Packages/com.unity.netcode@1.10/manual/rpcs.html)).
- **Addressing — the actual structural difference.** There is no object and no
  target entity. To send, you **create an entity** carrying the `IRpcCommand`
  component plus a `SendRpcCommandRequest` component whose `TargetConnection`
  field names a *connection entity*; "If `TargetConnection` is set to
  `Entity.Null`, the message is broadcast to all clients." On arrival,
  `RpcSystem` creates an entity holding the command struct plus
  `ReceiveRpcCommandRequest`, and consumer systems query for that pair
  (`SystemAPI.Query<RefRO<OurRpcCommand>, RefRO<ReceiveRpcCommandRequest>>()`),
  destroying the entity when done (rpcs.html 1.10 and
  [1.6](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/rpcs.html)).
  So: **the message is data, the recipient is a connection, and the handler is a
  system.** Three substitutions, none of them cosmetic.
- **The input path is deliberately *not* RPCs.** Player intent goes through the
  command stream: an `ICommandData` dynamic buffer keyed by `Tick`, or the
  higher-level `IInputComponentData` that codegen bakes into one. "A size limit
  of 1024 bytes is enforced on the command payload."
  `CommandSendPacketSystem` sends redundantly — "inputs from the previous `n`
  ticks," default 4. And the one-shot problem inside a redundant stream is
  solved explicitly: `InputEvent` fields "make sure that one-off events (such as
  those gathered by `UnityEngine.Input.GetKeyDown`) are synchronized properly
  with the server and registered exactly once, even when the exact input tick
  where the input event was first registered is dropped on its way to the
  server"
  ([command-stream.html](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/command-stream.html)).
  That is a *counted* one-shot inside a redundant unreliable stream — the same
  trick as Quake 3's event bits (§11) and directly applicable to Assisi's
  existing `InputCommandBuffer` redundancy.
- **Documented three-way guidance, which is exactly the state-first rule:** "Use
  RPCs to: Communicate high-level game flow events … Send one-off, non-predicted
  commands from the client to the server." "Use ghosts to: Replicate spatially
  local, ephemeral, and relevant per-entity data." And the reason: "RPCs are
  one-off events, and are therefore not automatically persisted," whereas "Ghost
  data persists for the lifetime of its ghost entity" (rpcs.html 1.6).
- **Reliability:** "RPCs are sent as reliable packets, while ghosts snapshots are
  unreliable (with eventual consistency)" (rpcs.html 1.6). Explicit ordering
  guarantees for RPCs against each other or against snapshots: **couldn't
  verify**.
- **Validation:** **couldn't verify** any ownership gate or validation hook on
  `IRpcCommand` in the fetched docs. Because the handler is a system that sees
  every request together with its connection entity, validation is *possible* in
  one place — but the framework does not provide it.
- **Versioning — the strongest story surveyed.** The handshake exchanges a
  `NetworkProtocolVersion` containing `NetCodeVersion`, `GameVersion`,
  `RpcCollectionVersion` — "A unique hash computed of all the RPC and commands"
  verifying "the server and client have the same messages and with compatible
  data and serialization" — and `ComponentCollectionVersion` for snapshots; on
  mismatch "the connection is forcibly closed"
  ([NetworkProtocolVersion API](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/api/Unity.NetCode.NetworkProtocolVersion.html);
  handshake timeout `ClientServerTickRate.HandshakeApprovalTimeoutMS`, default
  5000 ms —
  [network-connection.html](https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/network-connection.html)).
  **The RPC set is hashed into the handshake exactly like the component set.**
  That falls out for free from RPCs being data types.
- **Batching / payload limits for RPCs:** **couldn't verify** (the 1024-byte cap
  is documented for the command stream, not for RPCs).

## 5. Mirror

- **Authoring / addressing / direction:** `[Command]` (client→server),
  `[ClientRpc]` (server→all observers), `[TargetRpc]` (server→one connection);
  methods on a `NetworkBehaviour`, "cannot be static," arguments restricted (no
  "sub-components of game objects, such as script instances or Transforms")
  ([Remote Actions](https://mirror-networking.gitbook.io/docs/manual/guides/communications/remote-actions)).
  Attribute defaults in source: `CommandAttribute { channel = Channels.Reliable,
  requiresAuthority = true }`, `ClientRpcAttribute { channel = Channels.Reliable,
  includeOwner = true }`
  ([Attributes.cs](https://github.com/MirrorNetworking/Mirror/blob/master/Assets/Mirror/Core/Attributes.cs)).
  With `requiresAuthority = false`, a trailing `NetworkConnectionToClient sender
  = null` parameter is auto-filled with the caller — sender identity as a
  parameter, which is tidier than a global "who called me" query.
- **Validation:** "As rule of thumb, never trust the client!"; "In practice, you
  need to validate any client input in `[Commands]`"; "Cheaters usually modify
  the client to exploit games where the client is trusted with some decisions"
  ([Cheats & Anticheats](https://mirror-networking.gitbook.io/docs/security/cheating)).
  Guidance, not mechanism.
- **Versioning — a cautionary tale.** Remote calls are addressed by a **16-bit**
  stable hash of the fully-qualified method name (`ushort hash =
  functionFullName.GetStableHashCode16();`). Unknown ids fail silently *by
  design*, with the reasoning in a source comment: "note: no need to throw an
  error if not found. an attacker might just try to call a cmd with an rpc's
  hash etc. returning false is enough." Collisions are caught at registration
  with an error ending "…have the same hash. Please rename one of them. To save
  bandwidth, we only use 2 bytes for the hash, which has a small chance of
  collisions"
  ([RemoteCalls.cs](https://github.com/MirrorNetworking/Mirror/blob/master/Assets/Mirror/Core/RemoteCalls.cs)).
  Two lessons: **address messages by a hash of a name** (Assisi already does this
  for components), and **a per-call id space is a birthday problem you can avoid
  by hashing the whole set into the handshake instead** — which is what Netcode
  for Entities does.
- **Batching — the clearest documented policy found.** "Every message that you
  send will be batched until the end of the frame in order to minimize bandwidth
  and transport calls"; batching is bidirectional; batches target ~1200 bytes via
  `Transport.GetBatchThreshold()`; and "Mirror includes an 8 byte double
  precision timestamp in every Batch" rather than per message
  ([Timestamp Batching](https://mirror-networking.gitbook.io/docs/manual/general/timestamp-batching)).

## 6. FishNet

- **Authoring / direction:** `[ServerRpc]`, `[ObserversRpc]`, `[TargetRpc]`;
  "Fish-Net will generate serializers for your types automatically, including
  arrays and lists"; one method may carry both `[ObserversRpc]` and
  `[TargetRpc]`; `RunLocally` executes on the caller too; `DataLength` pre-sizes
  the buffer "to prevent garbage collection from resizing"
  ([Remote Procedure Calls](https://fish-networking.gitbook.io/docs/guides/features/network-communication/remote-procedure-calls)).
- **Reliability:** a trailing `Channel` parameter, `Channel.Reliable` (default) or
  `Channel.Unreliable`, with a graceful-degradation rule worth noting: "If you
  attempt to send a RPC as unreliable when the transport does not support it the
  RPC will default to reliable" (same page).
- **Ordering against state is *specified*, which almost nobody else does:**
  "SyncTypes always synchronize after remote procedure calls (RPC), even if you
  set them before calling the RPC," overridable per SyncType via
  `SyncTypeSettings`
  ([FAQ](https://fish-networking.gitbook.io/docs/guides/troubleshooting/frequently-asked-questions-faq)).
  Note this is the *same* ordering Unreal has (RPC first, state second) — stated
  as a documented gotcha rather than a guarantee anyone wanted.
- **The late-joiner band-aid:** `[ObserversRpc(BufferLast = true)]` "to
  automatically send latest values to newly joining clients" (RPC page). This is
  the most explicit admission in the survey that developers use RPCs for state
  anyway: rather than fix the misuse, FishNet gave the RPC a one-slot state
  buffer. **It is a state channel wearing an event's clothes**, and it is the
  precise anti-pattern Assisi's state-first rule exists to forbid.
- **Validation:** `RequireOwnership = false` on `[ServerRpc]` opens it to any
  client, with an optional `NetworkConnection` parameter identifying the caller
  (RPC page). Ownership gate only.
- **Versioning, batching:** **couldn't verify** for either.

## 7. Photon Fusion

All from [Fusion 2 RPCs](https://doc.photonengine.com/fusion/current/manual/data-transfer/rpcs)
unless noted.

- **Authoring:** `[Rpc]` on a method returning `void` or `RpcInvokeInfo`; the
  name must contain "RPC" as a prefix or suffix; an optional trailing `RpcInfo
  info = default` exposes `Tick`, `Source` (`PlayerRef`), `Channel`,
  `IsInvokeLocal`.
- **Direction as a permission pair — the best declarative version found.**
  `RpcSources` (who may invoke) and `RpcTargets` (who executes), each taking
  `All`, `Proxies`, `InputAuthority`, `StateAuthority`. Targeting one player is a
  parameter marked `[RpcTarget] PlayerRef`, and "Passing `PlayerRef.None` for the
  `[RpcTarget]` parameter will target the server!" **Authority is expressed as a
  declared source set rather than inferred from object ownership** — the same
  information Unreal derives from `AActor` ownership, but written down.
- **Tick alignment — a real fix, partially applied.** `TickAligned` (default
  true) delays execution on receivers until the sender's tick. This solves the
  "the event ran at a different simulation time on every machine" half of the
  ordering problem, though not the "the event refers to state that has not
  arrived" half.
- **Reliability:** `Reliable` (default), `Unreliable`, and `ReliableLargeData`
  (Fusion 2.1+) for payloads over the documented **512-byte** limit — with the
  honest cost that large-data RPCs "can't be tick aligned" and their "order is
  not preserved."
- **State vs events, stated bluntly:** "RPCs do not have an explicit state.
  Clients who late-join and clients who disconnect & reconnect will forget it
  ever happened." Persistent effects belong in `[Networked]` properties, which
  "automatically replicate their values from the State Authority Peer to all
  other Peers"
  ([Data Transfer overview](https://doc.photonengine.com/fusion/current/manual/data-transfer/data-transfer)).
- **Validation beyond `RpcSources`, versioning, and packet co-location with
  snapshots:** **couldn't verify**.

## 8. Photon Quantum — the deliberate opposite (again)

The state survey recorded Quantum as the system where the replication questions
dissolve. The same happens to RPCs, and the shape it dissolves *into* is
instructive.

- **There are no networked RPCs.** The only client→server verb is a **Command**:
  a class inheriting `DeterministicCommand` with an explicit `Serialize(BitStream
  stream)` and an `Execute(Frame frame)`, registered in `CommandSetup.User.cs`,
  sent with `Game.SendCommand(command)` (SDK 3.0) / `AddCommand` (3.1), and
  consumed inside the simulation via
  `frame.GetPlayerCommands<CommandSpawnEnemy>(player)`
  ([Commands](https://doc.photonengine.com/quantum/current/manual/commands)).
  Commands ride the deterministic input stream: they are "not required to be sent
  every tick and can be triggered in specific situations instead," and "Quantum
  Commands are fully reliable. By default, the server will always accept them and
  confirm it, regardless of the time at which they are sent."
- **"Events" in Quantum are not networked at all** — they are a
  simulation→view channel: "Events are a fire-and-forget mechanism to transfer
  information from the simulation to the view," declared in the `.qtn` DSL with
  the `event` keyword, and "Events do not synchronize anything between clients
  and they are fired by each client's own simulation"
  ([Game Events](https://doc.photonengine.com/quantum/current/manual/quantum-ecs/game-events)).
  Because prediction re-simulates frames, "it is possible to have events being
  triggered multiple times" (deduplicated by hash code), and non-`synced` events
  "will be either cancelled or confirmed once the predicted Frame from which they
  were fired has been verified"; the `synced` keyword defers dispatch to
  server-confirmed input.
- **Why this matters here:** Quantum's answer to "what is an RPC in an ECS" is
  *a reliable, serializable command object in the input stream, executed by the
  simulation* — plus a purely local presentation event that never crosses the
  wire. That is a two-channel design with **no general RPC surface at all**, and
  it is the same shape Assisi's `InputCommand` already has half of.

## 9. Godot — non-ECS, method-based, with a notably strict compatibility rule

- **Authoring:** `@rpc(mode, sync, transfer_mode, transfer_channel)` on a plain
  function; allowed values are mode `"authority"` / `"any_peer"`, sync
  `"call_remote"` / `"call_local"`, transfer_mode `"reliable"` / `"unreliable"` /
  `"unreliable_ordered"`, and `transfer_channel` "always has to be the 4th
  argument"
  ([@GDScript class ref](https://docs.godotengine.org/en/stable/classes/class_@gdscript.html)).
  Called with `rpc()` or `rpc_id(peer_id)`
  ([High-level multiplayer](https://docs.godotengine.org/en/stable/tutorials/networking/high_level_multiplayer.html)).
- **Addressing is a node path, and both peers must agree on it:** "For a remote
  call to be successful, the sending and receiving node need to have the same
  `NodePath`, which means they must have the same name," and "The signature of
  the RPC includes the `@rpc()` declaration, the function, return type, **and**
  the NodePath" (same page). This is the OOP model at its most literal — the
  scene tree *is* the address space.
- **Direction/authority:** `"authority"` means "Only the multiplayer authority
  can call remotely. The authority is the server by default"; `"any_peer"` means
  "Clients are allowed to call remotely. Useful for transferring user input,"
  and the callee identifies the caller with
  `multiplayer.get_remote_sender_id()`.
- **Reliability, with a subtle trap documented:** `"unreliable_ordered"` works by
  "ignoring packets that arrive later if another that was sent after them has
  already been received," so "Sending packets of variable size with this transfer
  mode can cause packet loss, since packets which are slower to arrive are
  ignored." Also: "The default channel with index 0 is actually three different
  channels - one for each transfer mode."
- **Validation:** the docs carry a security section — "treat all client input as
  untrusted," "Validate RPC arguments before applying them to the game state,"
  and "A common mistake is to let clients authoritatively decide important game
  states."
- **Versioning — the strictest per-call rule found, and it is loud about being
  vague:** both peers must declare the same RPCs, and "Both RPCs must have the
  same signature which is evaluated with a checksum of **all RPCs**." On
  mismatch, "the script may print an error or cause unwanted behavior," and —
  candidly — "The error message may be unrelated to the RPC function you are
  currently building and testing." A whole-set checksum with a diagnostic that
  cannot point at the culprit is the failure mode Assisi's `ProtocolSummary`
  (`NetProtocol.hpp:96-98`) already exists to avoid.

## 10. bevy_replicon — the one system that fixed the ordering-versus-state problem

- **Authoring:** message and event types are **registered on the `App` with a
  channel**, not attached to objects: `add_client_message()`,
  `add_mapped_client_message()`, `add_client_message_with()` (custom
  serialization) and the observer-based `add_client_event()` /
  `client_trigger()`; server→client mirrors them with `add_server_message()` /
  `add_server_event()` / `server_trigger()`, plus `add_shared_*` variants
  ([docs.rs crate root](https://docs.rs/bevy_replicon/latest/bevy_replicon/)).
- **Addressing:** nothing. Messages are app-level types; entity references
  travel *inside* the payload and must be declared for remapping — "Always use it
  for events that contain entities. Entities must be annotated with `#[entities]`"
  ([server_event](https://docs.rs/bevy_replicon/latest/bevy_replicon/shared/message/server_event/trait.ServerEventAppExt.html)).
- **Direction:** wrapper types rather than attributes. `ToClients<T> { targets:
  SendTargets, message: T }` with `SendTargets::All` ("Send to every client. This
  will also send the message locally to support listen server configuration."),
  `AllExcept(ClientId)`, `Single(ClientId)`; client→server arrives as
  `FromClient<T> { client_id, message }`
  ([server_message.rs](https://raw.githubusercontent.com/projectharmonia/bevy_replicon/master/src/shared/message/server_message.rs),
  [client_message.rs](https://raw.githubusercontent.com/projectharmonia/bevy_replicon/master/src/shared/message/client_message.rs)).
  Note that `SendTargets::All` explicitly includes the local listen-server path —
  the same problem NGO's `DeferLocal` addresses, solved by making local delivery
  part of the target semantics.
- **Reliability:** per-registration `Channel::Unreliable` ("Unreliable and
  unordered."), `Unordered` ("Reliable and unordered."), `Ordered` ("Reliable and
  ordered.")
  ([Channel](https://docs.rs/bevy_replicon/latest/bevy_replicon/shared/backend/channels/enum.Channel.html)).
- **The idea worth the whole survey — events are ordered *against the state
  stream*:** "By default, all server messages are buffered on server until server
  tick and queued on client until all insertions, removals and despawns (value
  mutations doesn't count) are replicated for the tick on which the message was
  written" (server_message.rs). An event that names an entity therefore cannot
  arrive before that entity exists, and an event about a state change cannot
  overtake the change. **Unreal specifies the opposite order** (§1). This is a
  genuine, cheap, structural fix to the single most common RPC bug class.
- **With a documented escape hatch, which is the honest half:**
  `make_message_independent` / `make_event_independent` — "you can mark it as
  independent to always emit it immediately. For example, a chat message - which
  does not hold references to any entities - may be marked as independent"
  (server_message.rs). The tick-sync costs latency, so the system lets you say
  when you do not need it.
- **Versioning:** by convention, not mechanism — "Make sure that the component,
  event and message registration order is the same on the client and server.
  Simply put all registration code in your 'shared' crate" (crate root). Weaker
  than Netcode for Entities' hash; the registration-order dependency is exactly
  the kind of thing Assisi's protocol hash would catch.
- **Validation:** sender identity is structural (`FromClient.client_id`), but
  **couldn't verify** any documented validation guidance.

## 11. Quake 3 — events delivered *by the state channel*

The oldest system here, and the one whose architecture most resembles what this
survey ends up recommending.

- **There is no general RPC for gameplay.** One-shot effects live *inside entity
  state*: `entityState_t` carries `int event;` (commented `// impulse events --
  muzzle flashes, footsteps, etc`) and `int eventParm;`
  ([q_shared.h](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/game/q_shared.h)),
  with `bg_public.h` explaining the design: "entity events are for effects that
  take place reletive to an existing entities origin. Very network efficient."
  ([bg_public.h](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/game/bg_public.h)).
- **Repeat disambiguation with two bits — the trick that makes events survive a
  delta channel.** "two bits at the top of the entityState->event field will be
  incremented with each change in the event so that an identical event started
  twice in a row can be distinguished. And off the value with ~EV_EVENT_BITS to
  retrieve the actual event number" (`EV_EVENT_BIT1 0x00000100`,
  `EV_EVENT_BIT2 0x00000200`, `EVENT_VALID_MSEC 300`, bg_public.h). The player's
  own events use a counter instead: `int eventSequence;` with
  `events[MAX_PS_EVENTS]` / `eventParms[MAX_PS_EVENTS]`, `MAX_PS_EVENTS 2`
  (q_shared.h).
- **Events with no owning entity get a temporary one.** `G_TempEntity` — "Spawns
  an event entity that will be auto-removed" — sets `e->s.eType = ET_EVENTS +
  event` and `freeAfterEvent = qtrue`; `G_AddEvent` "Adds an event+parm and
  twiddles the event counter"
  ([g_utils.c](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/game/g_utils.c)).
  So a gunshot is a one-frame entity in the snapshot, and it inherits every
  property of the state channel for free: delta compression, the acked baseline,
  relevancy, and self-healing under loss. Sanglard's summary of the channel:
  "any information that is not received on first transmission is not worth
  sending again because it will be too old anyway"
  ([Quake 3 network model](https://fabiensanglard.net/quake3/network.php)).
- **The separate reliable channel is a bounded command-string ring, and overflow
  drops the connection** — `SV_SendServerCommand` stores into
  `client->reliableCommands[reliableSequence & (MAX_RELIABLE_COMMANDS-1)]` and
  drops with "Server command overflow" when
  `reliableSequence - reliableAcknowledge == MAX_RELIABLE_COMMANDS + 1`
  ([sv_main.c](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/server/sv_main.c));
  the client dedups by sequence in `CL_ParseCommandString` — `if (
  clc.serverCommandSequence >= seq ) return;`
  ([cl_parse.c](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/client/cl_parse.c)).
  Note this is the same cliff as Unreal's `RELIABLE_BUFFER` (§1), reached by the
  same route, twenty years earlier.

## 12. Source, Tribes/TNL, Overwatch, Halo — the rest of the lineage

**Valve Source** — user messages are a hand-maintained registration table:
`CUserMessages::Register(const char *name, int size)` with `size` "-1 for
variable size", populated by a "Game specific registration function"
`RegisterUserMessages()`, dispatched by index through
`LookupUserMessage`/`DispatchUserMessage`
([usermessages.cpp, CS:GO SDK mirror](https://raw.githubusercontent.com/pmrowla/hl2sdk-csgo/master/game/shared/usermessages.cpp)).
An **index-addressed, manually-registered message table with no schema** is the
worst of both worlds for compatibility, and it is what produced the exploit in
§13. `CRecipientFilter`/`MakeReliable` specifics: **couldn't verify** (the Valve
wiki 403s automated fetches).

**Tribes / TNL** — the ancestor of the graded-guarantee idea. The Tribes paper's
event manager provides "guaranteed and non-guaranteed delivery of event objects…
Guaranteed events are also guaranteed to process in the order they were sent,"
over a connection layer that **never retransmits** — it only reports
delivery-status per packet, and "the events associated with the lost packet are
simply pushed onto the head of the event queue for re-transmission"
([Tribes networking model](https://www.gamedevs.org/uploads/tribes-networking-model.pdf)).
OpenTNL's `NetEvent` names three classes precisely: **GuaranteedOrdered**
("Event delivery is guaranteed and will be processed in the order it was sent
relative to other ordered events"), **Guaranteed** ("guaranteed and will be
processed in the order it was received"), **Unguaranteed** ("Event delivery is
not guaranteed - however, the event will remain ordered relative to other
unguaranteed events")
([TNL::NetEvent](https://opentnl.sourceforge.net/doxydocs/classTNL_1_1NetEvent.html)).
An event is a **class with `pack()`/`unpack()`/`process()`** — data plus a
handler, twenty-five years before Netcode for Entities' `IRpcCommand`. The
three-way split (reliable-ordered / reliable-unordered / unordered) is finer
than anything shipping in a game engine today except replicon's `Channel`.

**Overwatch (GDC 2017, Dan Reed)** — the ECS data point, and it is emphatic.
Statescript's one-shot **Actions are replicated through the state delta stream,
not through any message channel**: a `StatescriptDelta` contains, among other
things, an "Array of indices of States that changed" *and* an "Array of indices
of Actions that executed"; deltas are held per client until acknowledged ("A
StatescriptDelta is stored until all clients have acknowledged receipt of its
Command Frame"), and a packet is built by taking "a union of all
StatescriptDeltas in the Command Frame range" and serializing "the current values
of the objects referenced by this union" — with "If the Command Frame range
starts at 0, then just send the current values of all objects" as the full-state
path
([Reed, *Networking Scripted Weapons and Abilities in Overwatch*, GDC 2017 slides](https://media.gdcvault.com/gdc2017/Presentations/Reed_Dan_NetworkingScriptedWeapons.pdf);
text extracted from the PDF directly). The `SYNC_ALL` attribute governs both:
"With attribute: StatescriptStates transmit locally and remotely; StatescriptActions
transmit locally and remotely… Without attribute: StatescriptStates only transmit
locally; StatescriptActions do not transmit" (same deck). Statescript's own
"Events" are internal scheduled events (`List<StatescriptEvent*> m_futureEvents`,
`EnqueueTimerEvent`) and cost **0 bits** on remote entities in the deck's
efficiency table (431 bits local, 0 remote) — i.e. they are not networked.
Overwatch's honest caveat about the whole approach: "The eventual-consistency
networking model does not provide a perfect blow-by-blow replication."

**Halo: Reach (GDC 2011, Aldridge)** — the graded-guarantee argument in one
sentence: "some packets need guaranteed arrival (the game is over, client
requests grenade throw), some packets only need guaranteed ordering (player 3 is
at this position…), and some packets need no guarantees at all (sparks spawned
at this location, bullet hole appears here)." Cosmetic events are "prioritized
even more strictly, and are only sent once, so there is no guarantee that they
will arrive at all"
([Wolfire summary](https://www.wolfire.com/blog/2011/03/GDC-Session-Summary-Halo-networking)).

**O3DE** — worth one bullet because its authoring model is the closest existing
analogue to Assisi's reflectgen. RPCs are declared in **XML that generates C++**:
`<RemoteProcedure Name="SendConfirmUptime" InvokeFrom="Autonomous"
HandleOn="Authority" IsPublic="false" IsReliable="false"
GenerateEventBindings="false">` with `<Param Type="double" Name="UpTime" />`
children, alongside `<NetworkProperty ... ReplicateFrom="Authority"
ReplicateTo="Client" />` for state — roles are `Autonomous`, `Authority`,
`Client`
([Your First Network Component](https://raw.githubusercontent.com/o3de/o3de.org/main/content/docs/learning-guide/tutorials/multiplayer/first-multiplayer-component.md)).
Direction is a **declared (source, sink) role pair** like Fusion's
`RpcSources`/`RpcTargets`, and reliability is a per-declaration boolean. This is
Unreal's model expressed in a schema language rather than in C++ attributes —
proof that codegen does not force the ECS shape, it merely makes it available.

## 13. Roblox, Colyseus, Nakama — the "hostile client is the default" tier

- **Roblox** is the most exposed RPC surface in commercial use, and its docs say
  so. `RemoteEvent:FireServer()` / `FireClient(player, ...)` / `FireAllClients()`
  with `OnServerEvent` receiving the caller's `Player` as the first argument
  ([Remote events and callbacks](https://create.roblox.com/docs/scripting/events/remote)).
  The security page is unusually direct: "A determined exploiter has complete
  control over their local state and network traffic," they can "Fire or invoke
  RemoteEvents and RemoteFunctions at any frequency with arbitrary arguments,"
  and therefore "all critical logic must be validated server-side or run
  exclusively on the server" — with the server required to "Validate that the
  requested action is possible and permissible"
  ([Security tactics](https://create.roblox.com/docs/scripting/security/security-tactics)).
  Reliability is split into two instance types: `RemoteEvent` (reliable,
  ordered) and `UnreliableRemoteEvent` — "asynchronous, unordered and unreliable"
  with "Events with payloads larger than 1000 bytes are dropped," recommended
  for "ephemeral events… or for replicating continuously changing data," and
  "When you need ordering and reliability, use a `Class.RemoteEvent` instead"
  ([UnreliableRemoteEvent, official docs source](https://github.com/Roblox/creator-docs/blob/main/content/en-us/reference/engine/classes/UnreliableRemoteEvent.yaml)).
- **Colyseus** — message handlers on a room (`"action": (client, payload) => {}`,
  wildcard `"*"`, "You can only define a single callback per message type"),
  `client.send()` / `this.broadcast(..., { except: client })`, and validation via
  a `validate()` wrapper around a Zod schema, with invalid messages ignored
  ([Room](https://docs.colyseus.io/room)). One idea directly relevant here: the
  broadcast option **`afterNextPatch`**, which means "this message will arrive
  only after new state has been applied" — replicon's ordering guarantee, opt-in
  per call. State is the separate first-class mechanism, with late-join handled:
  "Clients receive the full state when they join the room"
  ([State](https://docs.colyseus.io/state)).
- **Nakama** — the honest two-tier version. Relayed matches forward opaque
  op-coded payloads with no inspection, and the docs disclaim everything: "there
  is no cheat detection, error correction, or other such functionality
  available"
  ([Relayed multiplayer](https://heroiclabs.com/docs/nakama/concepts/multiplayer/relayed/)).
  Authoritative matches exist "where you don't want to trust game clients," and
  make the non-forwarding explicit: "in authoritative matches received messages
  are not automatically rebroadcast to all other connected clients. Your match
  logic must explicitly call the `broadcast` function"
  ([Authoritative multiplayer](https://heroiclabs.com/docs/nakama/concepts/multiplayer/authoritative/)).
  Server-side registered RPCs (`RegisterRpc("my_unique_id", fn)`) are HTTP-callable
  and carry a scoping note worth copying: "if you want to scope functions to
  never be accessible from the client just return an error if you find a user ID
  in the context"
  ([Server framework basics](https://github.com/heroiclabs/nakama-docs/blob/master/docs/nakama/server-framework/basics.md)).

## 14. Non-game RPC — what transfers, and what does not

- **gRPC** brings three things game RPCs almost universally lack. **Deadlines**:
  "gRPC allows clients to specify how long they are willing to wait for an RPC to
  complete before the RPC is terminated with a `DEADLINE_EXCEEDED` error."
  **Cancellation**: "Either the client or the server can cancel an RPC at any
  time." And **explicit termination status**, with the caveat that "both the
  client and server make independent and local determinations of the success of
  the call, and their conclusions may not match"
  ([gRPC core concepts](https://grpc.io/docs/what-is-grpc/core-concepts/)).
  *What transfers:* very little of the request/response machinery — game RPCs are
  overwhelmingly one-way notifications, and a game that wants a reply has a state
  channel to put it on. *What does transfer:* the honesty about independent
  determinations, which is the same lesson as "fire-and-forget under loss."
- **Protobuf's versioning story is the part that transfers cleanly.** Fields are
  identified by **number**, and numbers "cannot be changed once your message type
  is in use"; "Adding new fields is safe… old binaries simply ignore the new
  field when parsing"; deleted numbers must be `reserved` and "Field numbers
  should never be reused"
  ([Protobuf language guide](https://protobuf.dev/programming-guides/proto3/)).
  This is *forward and backward compatibility by construction* — and it is
  explicitly **not** what Assisi does: the protocol hash refuses mismatched
  builds rather than tolerating them. That is the right trade for a same-binary
  target and the wrong one for a shipped game with staggered client updates; the
  choice should be re-examined the day clients update independently, not before.
- **Cap'n Proto RPC** contributes one idea worth naming, because it is the exact
  inverse of the game model. **Capability security**: "When a new object is
  created, only the creator is initially able to call it. When the object is
  passed over a network connection, the receiver gains permission to make calls
  – but no one else does," and "It is impossible for others to access the
  capability without consent of either the host or the receiver because the host
  only assigns it an ID specific to the connection over which it was sent"
  ([Cap'n Proto RPC](https://capnproto.org/rpc.html)). Every game system here
  does the opposite: any client may address any RPC on any object it can name,
  and the server filters afterwards by ownership. Unreal's owner check and
  Fusion's `RpcSources` are impoverished capability checks — a fixed, coarse,
  per-call ACL instead of an unforgeable handle. *Promise pipelining* ("The
  results of an RPC call are returned to the client instantly, before the server
  even receives the initial request!") does **not** transfer: it optimizes
  round-trip chains, and games do not chain RPCs.

## 15. Documented exploits — what actually goes wrong

The validation axis is not theoretical, so it gets its own section.

- **Among Us** is the canonical unvalidated-RPC failure. Tenable's analysis:
  "there is a total lack of server-side validation for player actions and
  interactions," with the missing check named — "validating that player and
  client IDs match up when communicating with the server" — enabling "Killing
  imposters," "Killing players when you're not the imposter," "Removing
  cooldowns for tasks, kills, calling meetings, etc.," and "Revealing imposters"
  ([Tenable TechBlog](https://medium.com/tenable-techblog/hacking-in-among-us-b43ea0fdd3d7)).
  Corroborating signal: the open-source replacement server *Impostor* lists
  "Server-sided anticheat" as a headline feature
  ([README](https://github.com/Impostor/Impostor/blob/master/README.md)).
  **The RPC surface was the entire attack surface**, because every gameplay verb
  was one.
- **CS:GO / Source — remote code execution through the message channel.** In
  `CSVCMsg_SplitScreen` the "`slot` field is used as an index for the array of
  splitscreen player objects located in the `.data` segment of `engine.dll`…
  without any bounds checks"; the out-of-bounds fetch yields a pointer, "a vtable
  is dereferenced and a function pointer is called," steered into ROP
  ([secret.club](https://secret.club/2021/05/13/source-engine-rce-join.html);
  [HackerOne #1070835](https://hackerone.com/reports/1070835), rated critical).
  Note the direction: **server→client**. A malicious *server* compromised
  clients. Every "the server validates client input" mitigation in this survey
  points the wrong way for this bug.
- **GTA Online — CVE-2023-24059**, a crafted network event: "Grand Theft Auto V
  for PC allows attackers to achieve partial remote code execution or modify
  files on a PC, as exploited in the wild in January 2023," CVSS 7.3
  ([NVD](https://nvd.nist.gov/vuln/detail/CVE-2023-24059)), reaching players
  "not in the same multiplayer lobby"
  ([BleepingComputer](https://www.bleepingcomputer.com/news/security/gta-online-bug-exploited-to-ban-corrupt-players-accounts/)).
- **Dark Souls III — CVE-2022-24126**, CVSS 9.8: "A buffer overflow in the
  NRSessionSearchResult parser… allows remote attackers to execute arbitrary code
  via matchmaking servers" ([NVD](https://nvd.nist.gov/vuln/detail/CVE-2022-24126)),
  the payload being a "chain of length-delimited data entries" with "Improper
  bounds checking on a stack buffer and data size field"
  ([PoC writeup](https://github.com/tremwil/ds3-nrssr-rce)).

**The pattern across all four:** the exploited thing was always a
*variable-shaped, attacker-influenced message parsed by hand-written code*.
Assisi's existing posture — every read through `BitReader` with a latched
failure state, hostile-input caps on every count, a protocol hash refusing
mismatched builds — is the correct starting point, and any RPC design must not
introduce a second, weaker parser beside it.

---

## 16. Comparison table

| System | Authoring surface | Addressed on | Direction / routing | Reliability | Order vs state | Validation | Versioning |
|---|---|---|---|---|---|---|---|
| **Unreal classic** | `UFUNCTION(Server/Client/NetMulticast, Reliable)` + `_Implementation` | a replicated `AActor`/subobject | derived from actor **ownership**; multicast → relevant clients | per-declaration; reliable buffer overflow **closes the connection** | **RPCs first, properties second**; unreliable multicast after | optional `WithValidation` + `_Validate` → disconnect | couldn't verify a per-RPC mechanism |
| **Unreal Iris** | unchanged | unchanged | unchanged | `FNetRPC` blobs; unreliable **unicast** enters the reliable queue for ordering, not resend | unchanged | unchanged | unchanged |
| **Unity NGO** | `[Rpc(SendTo.…)]` + `Rpc` name suffix, ILPP | a `NetworkBehaviour` on a `NetworkObject` | `SendTo` enum + `RpcTarget.Single/Group/Not` | reliable default, `RpcDelivery.Unreliable`; ordered **per NetworkObject only** | not documented | `RequireOwnership` gate only | `NetworkConfig.ProtocolVersion` + SHA256 config compare |
| **Unity Netcode for Entities** | **a struct** implementing `IRpcCommand` (codegen), or `IRpcCommandSerializer<T>` | **nothing** — an entity carrying the struct + `SendRpcCommandRequest{TargetConnection}` | connection entity, or `Entity.Null` = broadcast | reliable ("ghost snapshots are unreliable") | couldn't verify | couldn't verify any gate | **`RpcCollectionVersion` hash in the handshake**; mismatch closes the connection |
| **Mirror** | `[Command]` / `[ClientRpc]` / `[TargetRpc]`, weaved | a `NetworkBehaviour` | `requiresAuthority`, `includeOwner`, `NetworkConnection` first param | per-attribute `channel`, reliable default | couldn't verify | "never trust the client" guidance; authority flag | **16-bit hash of the method name**, silent on unknown, documented collision risk |
| **FishNet** | `[ServerRpc]` / `[ObserversRpc]` / `[TargetRpc]` | a `NetworkBehaviour` | `RequireOwnership`, `ExcludeOwner`, `NetworkConnection` first param | trailing `Channel` param; falls back to reliable if transport lacks unreliable | **documented: SyncTypes sync *after* RPCs** | ownership gate only | couldn't verify |
| **Photon Fusion** | `[Rpc(RpcSources, RpcTargets)]`, name must contain "RPC" | a `NetworkBehaviour` (or static on any `SimulationBehaviour`) | **declared source/target authority sets**; `[RpcTarget] PlayerRef` | `Reliable`/`Unreliable`/`ReliableLargeData`; **512-byte** cap | `TickAligned` (default on) defers to the sender's tick | `RpcSources` filter only | couldn't verify |
| **Photon Quantum** | `DeterministicCommand` class with `Serialize`/`Execute` | **nothing** — the input stream | client→server only; server→client does not exist | "fully reliable"; server always accepts | executes *inside* the simulation at a frame | the simulation is the validator | (deterministic build pairing; not examined) |
| **Godot** | `@rpc(mode, sync, transfer_mode, channel)` on a function | **a NodePath** (must match on both peers) | `authority` / `any_peer`; `rpc_id(peer)`; `get_remote_sender_id()` | reliable / unreliable / unreliable_ordered, per channel | not documented | manual sender checks; docs say validate arguments | **checksum of all RPCs**; error may be unrelated to the culprit |
| **bevy_replicon** | `add_client_message` / `add_server_event` etc., **registered types with a channel** | **nothing** — app-level; entities travel in the payload via `#[entities]` | `ToClients{targets}` / `FromClient{client_id}` | `Channel::Unreliable`/`Unordered`/`Ordered` | **queued on client until the message's replication tick is applied** | none documented | registration **order** must match — by convention |
| **O3DE** | `<RemoteProcedure InvokeFrom= HandleOn= IsReliable=>` XML → C++ | a network component on an entity | declared role pair (`Autonomous`/`Authority`/`Client`) | `IsReliable` boolean per declaration | not examined | not examined | not examined |
| **Quake 3** | none — `EV_*` in `entityState_t.event` + `G_TempEntity` | the entity (or a one-frame event entity) | whoever the snapshot goes to | **the snapshot's** — unreliable, self-healing; separate bounded reliable command ring | **identical** — events *are* state | server-side by construction | (same build assumed) |
| **TNL / Tribes** | `NetEvent` subclass with `pack`/`unpack`/`process` | nothing — the connection | per event object | **GuaranteedOrdered / Guaranteed / Unguaranteed** | events and ghosts are separate managers over one delivery-notification layer | per `process()` | (not examined) |
| **Overwatch** | none for one-shots — Actions ride `StatescriptDeltas` | the entity's Statescript instance | delta stream to acking clients | acked-baseline resend until confirmed | **identical** — actions travel with states | server-authoritative simulation | `SYNC_ALL` + compiled node/var tables |
| **Roblox** | `RemoteEvent` / `UnreliableRemoteEvent` instances | the instance in the DataModel | `FireServer` / `FireClient(player)` / `FireAllClients` | two instance types; unreliable drops >1000-byte payloads | not documented | docs: validate everything; assume arbitrary args at any frequency | none (dynamically typed) |
| **Colyseus** | `messages` map / `onMessage` by type string | the room session | `client.send` / `broadcast({except})` | WebSocket (reliable, ordered) | **opt-in `afterNextPatch`** | Zod `validate()` wrapper; server-authoritative state | couldn't verify |
| **Nakama** | numeric **op codes** on match data; registered server RPCs by string id | a match id / an RPC id | relayed (forwarded blind) or authoritative (`broadcast` explicit) | `reliable` boolean on broadcast | n/a (no built-in state sync) | authoritative match loop; relayed mode disclaims all checking | none (bare numbers) |

---

## 17. The three questions, answered

### Q1 — Is Unreal's model the de-facto standard?

**Yes, for one family, and that family is defined by having an object.** NGO,
Mirror, FishNet, Fusion, and Godot are all the same design with different
spellings: an attribute on a method of a networked object; direction and
reliability declared at the call site or the declaration; client→server gated by
ownership; recipients chosen by the object's observer/relevancy set. The
convergence is close enough that the differences are ergonomics — NGO's
`SendTo` enum and Fusion's `RpcSources`/`RpcTargets` are strictly better ways to
write the same information Unreal derives implicitly from actor ownership.

But "de-facto standard" overstates it in one direction: **the ECS systems and
the AAA in-house engines are not in that family**, and neither is Quantum, TNL,
or Quake. Counting shipped titles rather than middleware packages, "an RPC is a
method on a networked object" is a middleware convention, not an industry law.

### Q2 — Do ECS engines do something structurally different?

**Yes, and it is not cosmetic.** The common move is that **the unit of an RPC
becomes a data type rather than a function signature.** Netcode for Entities:
a struct implementing `IRpcCommand`, sent by creating an entity that carries it
plus `SendRpcCommandRequest`, received as an entity queried by a system.
bevy_replicon: a registered type with a channel and a `ToClients`/`FromClient`
wrapper. Quantum: a `DeterministicCommand` class in the input stream. TNL, in
1998: a `NetEvent` subclass with `pack`/`unpack`/`process`.

Four consequences that are architecture, not syntax:

1. **Versioning falls out.** Because the RPC is a data type, the RPC *set* is
   hashable, and Netcode for Entities hashes it into the handshake right beside
   the component set (`RpcCollectionVersion` next to
   `ComponentCollectionVersion`). The method-based systems either bolt on a
   whole-set checksum with an unhelpful diagnostic (Godot) or hash each method
   name to 16 bits and accept collisions (Mirror).
2. **Addressing decouples from the payload.** With no object to hang the call
   on, the recipient becomes an explicit field (a connection entity, a
   `SendTargets`) and any entity the message refers to becomes an explicit,
   *remappable* field in the payload (replicon's `#[entities]`). The OOP model
   fuses "who receives this" and "what this is about" into one pointer, which is
   why Unreal's ownership rules are a table you have to memorize.
3. **Dispatch becomes a query.** The handler sees every pending request of a
   type at once, together with its sender — which is the right shape for rate
   limiting, batching, and validation. The method model gives you one call, one
   sender, no context.
4. **The security principal changes.** Unreal's principal is the actor's owning
   connection; the ECS systems have no principal at all and must invent one
   (Fusion's `RpcSources` — but Fusion is not ECS).

The honest qualifier: **this is not a law of ECS, it is a consequence of
codegen.** Godot is not an ECS and uses methods. Unreal *Mass* is an ECS and
punts entirely — replication there is hand-written `ProcessClientReplication()`
into a client bubble (state survey, Mass entry). Overwatch is an ECS and has no
RPC channel at all. O3DE generates the OOP model from XML. What actually
determines the answer is *what your code generator already understands*. A
generator that understands data types makes the RPC a data type; that is exactly
Assisi's situation, and it is why the answer here is not "whatever Unreal did."

### Q3 — Has anyone fixed the known problems?

Partially, and the fixes are unevenly distributed. Scored against Unreal's
specific pains:

| Unreal's problem | Fixed by | How good |
|---|---|---|
| **Fire-and-forget under loss** | nobody, in the method family. Solved *architecturally* by Q3/Overwatch (events ride the acked state stream), and by Netcode for Entities' `InputEvent` (a counted one-shot inside a redundant unreliable stream) | the architectural fixes are total; the middleware answer is "use reliable and hope you don't overflow" |
| **Ordering vs state** | **bevy_replicon** — server messages queue on the client until the replication tick they were written on has been applied. Fusion's `TickAligned` does the timing half. Colyseus' `afterNextPatch` does it opt-in | **the single best idea in the survey.** Unreal specifies the opposite order and FishNet documents it as a gotcha |
| **Late joiners miss events** | nobody structurally. FishNet's `BufferLast` is a one-slot state buffer glued to an RPC | a band-aid that concedes the point. Every mature system's real answer is "then it was state" |
| **Validation boilerplate / weakness** | nobody. Unreal's optional `WithValidation` is still the high-water mark; everyone else ships an ownership flag | **an open problem industry-wide.** Roblox's docs are the most honest: assume arbitrary arguments at any frequency |
| **Ownership/authority footguns** | Fusion's `RpcSources`/`RpcTargets` and O3DE's `InvokeFrom`/`HandleOn` — authority *declared* instead of inferred | real ergonomic improvement, same semantics |
| **No batching / per-call cost** | Mirror (frame batching to ~1200 B with one timestamp per batch), replicon (buffered to the server tick), NGO (sent within the network frame) | solved, and boringly so |
| **RPC-vs-property confusion** | nobody mechanically; everybody documents it (NGO's "Should a player joining mid-game get that information?", Fusion's "Clients who late-join… will forget it ever happened") | **documentation is the state of the art.** Assisi's state-first rule, being a *rule* rather than a warning, is already ahead |

And the negative result worth recording plainly: **Iris changed nothing here.**
It rewrote how RPC bytes are scheduled (`FNetRPC` blobs, per-blob
`Reliable`/`Ordered` flags, unreliable unicast routed through the reliable queue
for ordering) and left the declaration model, the ownership routing, and the
validation story untouched. Anyone hoping "Epic already solved this in Iris"
should stop hoping.

---

## 18. What this suggests for Assisi

### The recommendation

**Do not build a method-based RPC system. Build two typed message channels, both
of which are extensions of things that already exist, and keep the state-first
rule as the primary gate.**

Concretely:

**1. Authoring is a struct, not a function.** A new annotation in the existing
grammar — spelled `AMSG()` here for discussion, the name is not the point —
applied to a plain struct, parsed by reflectgen's *existing* struct path.

This is the load-bearing recommendation and the reason is mechanical, not
aesthetic. `reflect_parser.py`'s field parser is a single regex over `Type
name;` declarations inside a brace body (`_FIELD_RE`, `reflect_parser.py:384`),
invoked by `_find_fields_in_body`, which raises if an `AFIELD` is not followed by
"a plain 'Type name;' declaration." It cannot parse a parameter list: no comma
handling, no multiple declarators, no defaults-with-commas, no return type.
Extending it to functions means a genuinely new signature parser, a new
`FunctionInfo`/`ParamInfo` model, and new codegen for dispatch — for the sole
benefit of writing `Server_DoThing(int32_t x)` instead of `struct DoThing {
int32_t x; }`. Meanwhile a struct gets serialization, the inspector, the JSON
and binary codecs, and **inclusion in `ProtocolHash`** for free. That last one
is not a minor convenience: it is the versioning story that Netcode for Entities
built deliberately (`RpcCollectionVersion`), that Godot approximates with a
whole-set checksum it cannot diagnose, and that Mirror gets wrong at 16 bits.
Assisi would get it by doing nothing.

The precedent is broad and independent: `IRpcCommand` (Unity), `NetEvent` (TNL),
`DeterministicCommand` (Quantum), registered message types (replicon). Four
systems, four decades, same answer.

**2. Client→server is one channel: reliable intent, on the `Control` lane,
extending the input path.** Quantum's Command is exactly this and nothing else;
Netcode for Entities' guidance is "Send one-off, non-predicted commands from the
client to the server." Assisi already has the hardened half — `InputCommand` is
tick-stamped, rate-limited, and clamped through `ClampInputCommand`
(`InputCommand.hpp:75-91`), with `InputCommandQueue` deduplicating and dropping
stale ticks. An intent message is the same pipe with reliable delivery and a
type tag instead of a fixed struct.

Why one channel and not per-entity RPCs: it produces **one receive site to
validate**, which is the only structural answer to §15's exploit pattern. The
server's message loop sees sender, type, and tick together, and can rate-limit
per connection per type before dispatch. Compare Unreal, where every `_Validate`
is a separate optional opportunity to forget, and Among Us, where the absence of
one central check *was* the vulnerability.

Two rules for that site, both stricter than the input path's:
- **Reject and log; do not clamp.** Clamping is right for a controller axis (a
  stick can legitimately saturate). It is wrong for an intent, where an
  out-of-range field means the client is lying or the builds disagree — and
  clamping converts a detectable attack into a silently-accepted one.
- **Rate-limit per connection per message type**, with the counter visible in
  the Network panel. Unreal needed `FRPCDoSDetection` retrofitted; the shape is
  known in advance here.

**3. Server→client is a snapshot section, tick-stamped, and applied *after* the
entity blocks it refers to.** This is replicon's guarantee — "queued on client
until all insertions, removals and despawns … are replicated for the tick on
which the message was written" — and it is close to free in this codebase for a
reason worth stating: the snapshot already carries `serverTick` and
`baselineTick` (`NetProtocol.hpp:162-184`), and the wire order already puts
entity component blocks before the body-state section
(plan-v4 §3.3). A message section appended after both, referring to NetIds
established earlier *in the same packet*, inherits the ordering property from
the framing rather than from a queue.

Where it is not free: a message about an entity whose spawn was budget-cut must
be **held, not dropped** — the same gate plan-v4 §3.3 already specifies for body
states ("only included for an entity the connection already has… or whose entity
block was written into this same snapshot"), but with the opposite resolution,
because a state can wait for the next tick and an event cannot be regenerated.
That means a small per-connection pending-message queue keyed by NetId, drained
when the NetId becomes known. Copy replicon's escape hatch too: a per-message
`independent` flag for messages that name no entity (a chat line, a round-start
banner), which skips the queue entirely.

**4. Addressing is a field, not a receiver.** If a message is about an entity, it
carries `NetId` as a declared field — the thing that makes rule 3 expressible,
and the thing replicon needs `#[entities]` for. There is no "call this RPC on
that entity," because there is no object; the handler is a function registered
against the message type, which sees the whole batch.

**5. Reliability is per message *type*, declared once, and deliberately
restricted.** The survey's three-way vocabulary is TNL's and replicon's
(reliable-ordered / reliable-unordered / unordered). Ship **two**: reliable on
`Control` for intents and rare authoritative announcements, unreliable inside
the snapshot for cosmetics that may be lost. Do not build general
reliable-ordered messaging: both Quake 3 (`MAX_RELIABLE_COMMANDS`, "Server
command overflow") and Unreal (`RELIABLE_BUFFER` 256, connection closed) show
the same cliff, reached the same way, and Unreal's mitigation is asking every
call site to budget against a global.

**6. Keep the state-first rule, and note that it is already ahead of the
field.** Every mature system in this survey warns against events-for-state in
prose: NGO's "Should a player joining mid-game get that information?", Fusion's
"Clients who late-join and clients who disconnect & reconnect will forget it
ever happened," vorixo's "The newly spawned client won't see the new mesh,"
Netcode for Entities' "RPCs are one-off events, and are therefore not
automatically persisted." Nobody enforces it, and FishNet's `BufferLast` shows
where the pressure leads when you do not. Assisi's advantage is having written
it down as a rule before the mechanism existed. Two cheap reinforcements: the
authoring cost of a message (declare a struct, register a handler) should stay
*higher* than the cost of marking a field replicable, and the panel should show
message rates per type so "we are streaming state through events" is visible
rather than inferred.

### What to *not* build, with reasons

- **No arbitrary per-call recipient lists — but yes to declarative recipient
  classes.** *(Revised: the first version of this bullet rejected recipient
  selection because Assisi has no relevancy system yet. That was reasoning from
  a current gap; re-derived here on the merits, assuming interest management
  will exist.)* The surveyed systems that scale all converge on the same split:
  the caller picks a **class** of recipients, and the *membership* of that class
  is computed by the engine's relevancy/ownership machinery, never by gameplay
  code. Unreal's `NetMulticast` reaches "all connected clients that the Actor is
  relevant to" — the caller cannot enumerate connections
  ([WizardCell](https://wizardcell.com/unreal/multiplayer-tips-and-tricks/));
  NGO's `SendTo` enumerates classes (`Everyone`, `Owner`, `NotOwner`, `NotMe`,
  `NotServer`…), not lists (§3); Fusion's `RpcTargets` are authority classes
  (§7); Colyseus takes a broadcast with an `except` clause (§13). So the
  recommendation, stated positively: ship **three declarative forms** —
  *all-relevant* (default: the message about entity E goes to every connection
  whose relevant set contains E, which the snapshot-section design produces
  structurally, since the message section is built per connection alongside the
  entity blocks), *directed* (one connection — the owner case), and *except*
  (all-relevant minus the instigator, for events the instigator already
  predicted locally — Colyseus's `except`, NGO's `NotMe`). What stays rejected,
  now for merits rather than absence: a gameplay-computed connection list
  bypasses the relevancy boundary — it is the API through which an event leaks
  what state filtering withholds (the wallhack-shaped failure; see the
  relevancy/ownership companion survey) — and it duplicates, in every call
  site, the recipient computation that relevancy owns in one place.
- **No per-message `_Validate` hook.** It is Unreal's shape, it is optional
  there, and the single dispatch site does the job better.
- **No request/response or return values.** No game system surveyed has them
  (gRPC's do not transfer, §14); a reply is state, and Assisi has a state
  channel.
- **No buffered/last-value messages.** That is `BufferLast`, and it is the
  state-first rule's failure mode with a feature name.

### The tradeoffs, named

- **Verbosity.** `struct Detonate { NetId target; };` plus a registered handler
  is more typing than `UFUNCTION(NetMulticast) void Detonate()`. This is real,
  and it is the cost every struct-based system pays. It is also the cost that
  buys the hash-in-the-handshake and the single validation site.
- **Latency on the held-message path.** A message about an entity whose spawn
  was budget-cut waits a snapshot interval or more. Correct, but a hit-spark
  should not wait — hence the `independent` flag, and hence the rule that
  cosmetics that reference nothing should be marked as such.
- **No per-entity dispatch.** "Run this on that entity" is a NetId lookup in the
  handler. Acceptable; it is what a query-shaped handler does anyway.
- **The protocol hash refuses rather than tolerates — and which is right is a
  deployment-model question, not a project-size one.** Adding a message type
  changes the hash and mismatched builds refuse to pair. *(Re-derived: the point
  is not "Assisi is same-binary today so refuse is fine." It is that refuse and
  tolerate are each correct for a different deployment model, and the choice
  should track which model the engine is actually serving.)* When client and
  server are *meant* to be the same build — PIE, a hosted listen server, matched
  co-op — refuse-on-mismatch is strictly safer and free: a mismatch there is a
  bug or an attack, never an intended state, and the one thing worse than
  refusing is silently misparsing (§15's exploit pattern is hand-written parsers
  meeting unexpected bytes). When clients are *meant* to update independently of
  servers — a shipped title with a patch cadence, a public server someone else
  runs — refuse-on-mismatch becomes a liveness failure: every server update
  locks out every un-updated client. That case needs Protobuf's discipline
  (§14): field-numbered messages, additive-only changes, unknown fields ignored,
  numbers never reused — forward/backward compatibility *by construction*. The
  design implication for the message channel specifically: **make the wire form
  tolerate-capable from the start even while the handshake refuses.** A message
  is a reflected struct; if its fields are addressed by a stable id (the way
  Protobuf uses field numbers, the way Assisi already persists component *names*
  rather than indices — opt-in plan D5), then the same message type can later be
  read by a newer or older peer that knows a superset/subset of its fields, and
  the refuse-vs-tolerate decision becomes a handshake *policy* rather than a wire
  *format* rewrite. Building the strict handshake now is right; building a wire
  format that *cannot* later tolerate is the avoidable mistake, and it is
  avoidable for free by addressing fields stably — which the reflection system
  does already.
- **This design cannot express "call a method on a remote object."** If a future
  scripting or blueprint layer wants that shape, it will have to be built *on
  top* — a generated message type per scripted event. Worth knowing before the
  blueprint rebuild (docs/blueprint-system-concept.md) rather than after.

### One thing the survey says the deferral got right

plan-v4 §5 defers RPCs with "validation is security surface, not a rider." §15
is that sentence with evidence: four documented incidents, all of them a
variable-shaped attacker-influenced message meeting hand-written parsing, and an
industry whose best available answer is an optional macro. The deferral was
correct, and the design when it lands should treat the *single validated
dispatch site* as the feature, with the message format as the implementation
detail.

---

## 19. What I could not verify

- **Unreal's official RPC and Iris documentation pages render client-side** and
  returned empty content to automated fetch (`dev.epicgames.com` 5.x pages) or
  HTTP 403 (`docs.unrealengine.com` 4.27 mirror). Unreal claims here are sourced
  from the 4.27 doc page as surfaced in search, Cedric Neukirchen's compendium,
  WizardCell, and vorixo — all community, all consistent with each other — plus
  the *Replicated Object Execution Order* page, which did fetch cleanly and is
  the source for every ordering claim. Treat exact specifier casing (`Reliable`
  vs `reliable`) as unconfirmed.
- **Unreal's RPC versioning.** No documented per-RPC compatibility mechanism
  found. Whether Unreal detects a mismatched RPC set at all, or only gates on
  engine/network version, is unresolved.
- **Unreal's `FRPCDoSDetection` defaults.** The class and
  `ERPCDoSEscalateReason` exist in the API docs; its default configuration and
  whether it is on in a stock build were not verified.
- **Iris's official RPC documentation.** The Epic pages would not fetch; the
  `FNetRPC`/`NetBlob`/`ENetBlobFlags` mechanism and the "unreliable unicast goes
  in the reliable queue" behaviour come from an Epic staff reply on the UE
  forums, which is authoritative on behaviour but is not documentation.
- **Netcode for Entities:** no ownership gate or validation hook for
  `IRpcCommand` found; no explicit RPC ordering guarantee; no per-RPC size limit
  (the 1024-byte cap is the *command stream's*); no RPC batching policy.
- **NGO:** per-method hash-mismatch behaviour; RPC ordering against
  `NetworkVariable` deltas; the current batching implementation (the RFC found is
  MLAPI-era design, not shipped-implementation documentation); the default value
  of the legacy `ServerRpcAttribute.RequireOwnership`.
- **Mirror:** documented ordering of remote calls against SyncVar state; an
  explicit late-joiner warning.
- **FishNet and Fusion:** version-mismatch behaviour for both; RPC/snapshot
  packet co-location for both.
- **bevy_replicon:** any documented validation guidance; whether messages share
  packets with replication data beyond the "buffered until server tick" statement.
- **Godot:** exact failure behaviour on RPC checksum mismatch (docs say only "may
  print an error or cause unwanted behavior"); per-call overhead.
- **Roblox:** official RemoteEvent bandwidth or queue limits (only the
  `UnreliableRemoteEvent` 1000-byte drop threshold is documented; a devforum
  thread disputes the figure, which is itself unofficial).
- **Colyseus / Nakama:** version-mismatch behaviour; message size and rate
  limits.
- **Valve Source:** `CRecipientFilter`, `MakeReliable`, `FCVAR_SERVER_CAN_EXECUTE`
  semantics, and the concrete user-message name/size table — the Valve wiki 403s
  automated fetches and the message table lives in a game DLL file not fetched.
  The registration mechanism itself (`Register(name, size)`, `-1` for variable
  size) is verified from the CS:GO SDK mirror.
- **CVE identifiers for the two 2021 secret.club Source RCEs.** The
  `CSVCMsg_SplitScreen` bug is confirmed via the blog and HackerOne #1070835; no
  primary CVE record was confirmed for it.
- **O3DE** was covered from one tutorial page only; its reliability, validation,
  and versioning behaviour were not examined.

## Sources

- Unreal — Replicated Object Execution Order: <https://dev.epicgames.com/documentation/en-us/unreal-engine/replicated-object-execution-order-in-unreal-engine> · RPCs (4.27): <https://dev.epicgames.com/documentation/en-us/unreal-engine/rpcs?application_version=4.27> · Iris system: <https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine> · `ERPCDoSEscalateReason`: <https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/ERPCDoSEscalateReason> · Iris RPC queue thread (Epic staff reply): <https://forums.unrealengine.com/t/iris-unreliable-rpcs-such-as-clientack-are-being-treated-as-reliable/2609151>
- Unreal, community — Neukirchen, *Remote Procedure Calls*: <https://cedric-neukirchen.net/docs/multiplayer-compendium/remote-procedure-calls/> · WizardCell, *Multiplayer Tips and Tricks*: <https://wizardcell.com/unreal/multiplayer-tips-and-tricks/> · vorixo, *Correct stateful replication*: <https://vorixo.github.io/devtricks/stateful-events-multiplayer/> · vorixo, *Multiplayer data streaming*: <https://vorixo.github.io/devtricks/data-stream/> · Cyrex, *RPC Validation*: <https://cyrex.tech/rpc-validation-with-unreal-engine/>
- Unity NGO — RPC: <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/advanced-topics/message-system/rpc.html> · RPC params: <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/advanced-topics/message-system/rpc-params.html> · Reliability: <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/advanced-topics/message-system/reliability.html> · RPC vs NetworkVariable: <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.5/manual/learn/rpcvnetvar.html> · Messaging system: <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.7/manual/advanced-topics/messaging-system.html> · `NetworkConfig`: <https://docs.unity3d.com/Packages/com.unity.netcode.gameobjects@2.2/api/Unity.Netcode.NetworkConfig.html>
- Unity Netcode for Entities — RPCs: <https://docs.unity3d.com/Packages/com.unity.netcode@1.10/manual/rpcs.html> and <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/rpcs.html> · Command stream: <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/command-stream.html> · `NetworkProtocolVersion`: <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/api/Unity.NetCode.NetworkProtocolVersion.html> · Network connection: <https://docs.unity3d.com/Packages/com.unity.netcode@1.6/manual/network-connection.html>
- Mirror — Remote Actions: <https://mirror-networking.gitbook.io/docs/manual/guides/communications/remote-actions> · Cheats & Anticheats: <https://mirror-networking.gitbook.io/docs/security/cheating> · Timestamp Batching: <https://mirror-networking.gitbook.io/docs/manual/general/timestamp-batching> · `Attributes.cs`: <https://github.com/MirrorNetworking/Mirror/blob/master/Assets/Mirror/Core/Attributes.cs> · `RemoteCalls.cs`: <https://github.com/MirrorNetworking/Mirror/blob/master/Assets/Mirror/Core/RemoteCalls.cs>
- FishNet — RPCs: <https://fish-networking.gitbook.io/docs/guides/features/network-communication/remote-procedure-calls> · Communicating: <https://fish-networking.gitbook.io/docs/guides/high-level-overview/terminology/communicating> · FAQ: <https://fish-networking.gitbook.io/docs/guides/troubleshooting/frequently-asked-questions-faq>
- Photon Fusion — RPCs: <https://doc.photonengine.com/fusion/current/manual/data-transfer/rpcs> · Data Transfer: <https://doc.photonengine.com/fusion/current/manual/data-transfer/data-transfer>
- Photon Quantum — Commands: <https://doc.photonengine.com/quantum/current/manual/commands> · Game Events: <https://doc.photonengine.com/quantum/current/manual/quantum-ecs/game-events>
- Godot — `@rpc` in @GDScript: <https://docs.godotengine.org/en/stable/classes/class_@gdscript.html> · High-level multiplayer: <https://docs.godotengine.org/en/stable/tutorials/networking/high_level_multiplayer.html> · MultiplayerSynchronizer: <https://docs.godotengine.org/en/stable/classes/class_multiplayersynchronizer.html>
- bevy_replicon — crate docs: <https://docs.rs/bevy_replicon/latest/bevy_replicon/> · `Channel`: <https://docs.rs/bevy_replicon/latest/bevy_replicon/shared/backend/channels/enum.Channel.html> · `server_message.rs`: <https://raw.githubusercontent.com/projectharmonia/bevy_replicon/master/src/shared/message/server_message.rs> · `client_message.rs`: <https://raw.githubusercontent.com/projectharmonia/bevy_replicon/master/src/shared/message/client_message.rs>
- O3DE — Your First Network Component: <https://raw.githubusercontent.com/o3de/o3de.org/main/content/docs/learning-guide/tutorials/multiplayer/first-multiplayer-component.md>
- Roblox — Remote events and callbacks: <https://create.roblox.com/docs/scripting/events/remote> · Security tactics: <https://create.roblox.com/docs/scripting/security/security-tactics> · `UnreliableRemoteEvent` (docs source): <https://github.com/Roblox/creator-docs/blob/main/content/en-us/reference/engine/classes/UnreliableRemoteEvent.yaml>
- Colyseus — Room: <https://docs.colyseus.io/room> · State: <https://docs.colyseus.io/state>
- Nakama — Authoritative multiplayer: <https://heroiclabs.com/docs/nakama/concepts/multiplayer/authoritative/> · Relayed multiplayer: <https://heroiclabs.com/docs/nakama/concepts/multiplayer/relayed/> · Server framework basics: <https://github.com/heroiclabs/nakama-docs/blob/master/docs/nakama/server-framework/basics.md>
- Quake 3 — `bg_public.h`: <https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/game/bg_public.h> · `q_shared.h`: <https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/game/q_shared.h> · `g_utils.c`: <https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/game/g_utils.c> · `sv_main.c`: <https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/server/sv_main.c> · `cl_parse.c`: <https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/client/cl_parse.c> · Sanglard: <https://fabiensanglard.net/quake3/network.php>
- Source — `usermessages.cpp` (CS:GO SDK mirror): <https://raw.githubusercontent.com/pmrowla/hl2sdk-csgo/master/game/shared/usermessages.cpp> · Source Multiplayer Networking: <https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking>
- Tribes / TNL — Frohnmayer & Gift: <https://www.gamedevs.org/uploads/tribes-networking-model.pdf> · `TNL::NetEvent`: <https://opentnl.sourceforge.net/doxydocs/classTNL_1_1NetEvent.html>
- Overwatch — Reed, *Networking Scripted Weapons and Abilities* (GDC 2017 slides): <https://media.gdcvault.com/gdc2017/Presentations/Reed_Dan_NetworkingScriptedWeapons.pdf>
- Halo: Reach — Aldridge, *I Shot You First* (GDC 2011), Wolfire summary: <https://www.wolfire.com/blog/2011/03/GDC-Session-Summary-Halo-networking>
- gRPC core concepts: <https://grpc.io/docs/what-is-grpc/core-concepts/> · Protobuf language guide: <https://protobuf.dev/programming-guides/proto3/> · Cap'n Proto RPC: <https://capnproto.org/rpc.html>
- Exploits — Among Us (Tenable): <https://medium.com/tenable-techblog/hacking-in-among-us-b43ea0fdd3d7> · Impostor: <https://github.com/Impostor/Impostor/blob/master/README.md> · CS:GO RCE (secret.club): <https://secret.club/2021/05/13/source-engine-rce-join.html> · HackerOne #1070835: <https://hackerone.com/reports/1070835> · CVE-2023-24059: <https://nvd.nist.gov/vuln/detail/CVE-2023-24059> · CVE-2022-24126: <https://nvd.nist.gov/vuln/detail/CVE-2022-24126> · DS3 PoC writeup: <https://github.com/tremwil/ds3-nrssr-rce>
- In-repo — docs/replication-plan-v4.md · docs/replication-optin-plan-v1.md · docs/replication-research-ecs-survey.md · docs/research/networking/r1-engine-case-studies.md


---

## Postscript — what Assisi chose, and why

Written after the build (docs/replication-messaging-relevancy-plan-v1.md,
M3–M5), so the record says what happened rather than what was recommended.

**Structs, not functions — §18's load-bearing recommendation, adopted whole.**
`AMSG(direction, reliability)` on a plain struct goes through reflectgen's
existing field path, so a message gets the binary codec, the JSON codec, the
inspector, and a place in the protocol hash without a line of new machinery.
That last one is the reason: declaring a message is a *versioning event*, so two
builds that disagree refuse to connect instead of misparsing each other. It is
the story Unity built deliberately with `RpcCollectionVersion`, Godot
approximates with an undiagnosable whole-set checksum, and Mirror gets wrong at
sixteen bits — and here it falls out of the annotation rather than being
engineered.

**Where the survey was overruled.** §18 proposed direction and reliability as
defaulted arguments. Both are mandatory and positional instead: the declaration
states the whole wire contract with nothing to memorise, and no later change of
default can silently reclassify a message written under the old one. Unreliable
*intents* were also added as a first-class cell — the survey treated
client→server as necessarily reliable, and the spammy freshest-wins asks (map
pings, look-here markers) are worse served by a resend than by a loss.

**Where the survey was corrected by the code.** §18 called the length-prefixed
wire form "tolerate-capable". It is not: with dense ids a peer holding a
different message set steps the right number of bytes and then dispatches the
wrong type for every id after the divergence. The prefix buys *skip*. Real
tolerance needs stable name-derived ids and a handshake policy that permits the
mismatch, and the prefix is what keeps that a policy change rather than a format
rewrite — recorded as a seam, not built.

**The exploit pattern drove the shape.** All four documented incidents in §15
are attacker-made messages meeting hand-written parsing across many receive
sites, so the single validated dispatch site is what was built and intents are
what happens to travel through it. Eight steps, ordered so a flood costs a
comparison rather than a parse. Field ranges are rejected, never clamped — the
input path clamps because a stick can saturate, while an out-of-range intent
field means the client is lying or the builds disagree, and clamping converts a
detectable attack into a silently accepted one.

**The reliable-buffer cliff was respected rather than mitigated.** Q3 and Unreal
both hit it, and both mitigations amount to asking every call site to budget
against a global. There are two forms and no general reliable-ordered messaging:
unreliable events ride the snapshot after the entity blocks, which buys
replicon's ordering guarantee out of the framing itself, and reliable
announcements carry a tick stamp the client defers against.

**The host is not a special case.** A listen server's player is not a
connection, which would have made it the one participant whose messages skipped
every check and whose path no fuzz test covered. Its intents are encoded and
re-decoded to reach the identical dispatch site, and it is not exempt from
validation — trusted is not the same as correct.
