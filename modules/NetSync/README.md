# NetSync

Server-authoritative replication: what crosses the wire, who is told about it,
and what a client is allowed to say back.

The whole model is five sentences, and every mechanism in this module is one of
them:

> **Gates decide what an entity says. Relevancy decides who is listening.
> Priority decides who is heard first. Control decides who is speaking.
> Messages are for what happened, never for what is.**

## The five sentences, and where each one lives

**Gates** — five of them, each at the scope that owns the decision, so an engine
module cannot set a game's network policy by editing one of its own headers:
`ACOMP(replicable)` grants a capability, `game.json`'s `neverReplicate` is the
game's veto, the `Replicated` marker is the entity's opt-in, its `excluded` mask
is that entity's opt-out, and `AFIELD(norep)` keeps a field off the wire while
still saving it to disk. See `Assisi/Core/Reflect/Annotations.hpp` and
docs/replication-optin-plan-v1.md.

**Relevancy** — one sorted `NetId` set per connection, intersected with the live
set at four seams in `SendSnapshot` before anything else runs. No provider is the
default and costs nothing at all; `DistanceRelevancy` is the one that ships. The
engine's promise is that an entity outside a connection's set contributes **zero
bytes** to it — no component blocks, no body state, no messages about it — and
that promise is what makes a game-side information boundary (line of sight, fog
of war) a provider rather than an engine feature.

**Priority** — a Tribes-lineage accumulator, inert until the byte budget binds
and then degrading correction *frequency* smoothly and per object, steered by an
authored number rather than by whichever NetId happened to be lowest. Filtering
happens strictly before it: a prioritizer that also filters is two axes fused
into one number.

**Control** — one component, `ControlledBy`, deliberately not called `Owner`. It
does three jobs — input binding, directed-message addressing, disconnect cleanup
— and state authority is emphatically not one of them. It is written by the
server at runtime and never authored, because a client id belongs to one
session.

**Messages** — plain structs under `AMSG(direction, reliability)`, so they get
the codecs, the inspector, and a place in the protocol hash from the machinery
components already use. Clients send *intents* through exactly one validated
dispatch site; the server sends *events*, either riding the snapshot (where
ordering against the entity they name is free) or as tick-stamped announcements
the client defers against.

## Reading order

- docs/replication-plan-v4.md — transport, snapshots, baselines, body state.
- docs/replication-optin-plan-v1.md — the five gates.
- docs/replication-messaging-relevancy-plan-v1.md — relevancy, control, messages.

Each supersedes the previous one only where it says so; all three are current.
