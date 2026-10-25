# Multiplayer basics

Assisi has networking built in. One machine is the **host** (the server), which
runs the real game, and the others are **clients** that join it and see the same
world.

> **Still being built:** multiplayer works, but it's one of the youngest parts of
> the engine, and it will change more than anything else in this book.

## Trying it in the editor

The **Game** panel has a network mode dropdown next to **Run**:

| Mode | What happens |
|---|---|
| **Standalone** | Normal single-player play. |
| **Host** | The editor plays as a server that others can join. |
| **Host + 1 / 2 / 3 clients** | Plays as the host, and opens that many extra client windows that join it automatically. The quickest way to test multiplayer. |
| **Join...** | Connect to a host at an address. |

Pause isn't available during a networked session.

## What gets sent over the network

Sending a whole world every frame would be far too much data, so nothing is sent
unless it's allowed at every level:

1. **The component type allows it.** Only components declared
   `ACOMP(replicable)` can travel. `Transform`, `MeshRenderer` and
   `Collider` are replicable, for example.
2. **Your game doesn't block it.** `neverReplicate` in
   `assets/config/network.json` blocks a component type for your whole game.
3. **The entity opts in.** Only entities with a **`Replicated`** component are
   sent at all.
4. **The entity doesn't exclude that component.** `Replicated` has a list of
   components to keep local on that one entity.
5. **The field isn't held back.** `AFIELD(norep)` marks a field that's saved to
   disk but never sent.

In the editor, add `Replicated` to an entity to make it networked. The inspector
then shows a checklist of which of its components are sent.

For your own components:

```cpp
ACOMP(replicable)
struct Health
{
    AFIELD() float current = 100.f;
    AFIELD(norep) float regenDelaySeconds = 3.f; // only the host needs this
};
```

## How it stays in sync

Every machine runs the same physics simulation, so a client can predict most of
what happens by itself. The host sends only what changed since the last update
each client confirmed, and sends corrections when a client drifts. Corrections
are eased in so they don't look like teleports.

When two machines connect, they first compare a fingerprint of every replicable
component's layout. If the builds differ, they refuse to connect rather than
misreading each other's data. **Host and clients must run the same build.**

## Players

Each person in a session controls their own character, and sees everyone
else's. The host hands out the characters:

- **The host keeps the level's own player.** The first character in the level
  marked `Player` (see [Players](players.md)) becomes the host's.
- **Everyone who joins gets a new one**, made at a **`PlayerStart`**. Give an
  entity a `PlayerStart` and a `Transform`, and set its `blueprint` to the
  player blueprint to make, `blueprints/Player.abp` by default. With several
  starts, players are spread over them in the order they joined. A level with
  no start gives late joiners nothing, and the log says so.
- **A player who leaves takes their character with them**, camera and all.

`levels/NetPile.alvl` has three starts. To try it, open it, pick **Host + 2
clients** and press **Run**: each window walks its own character, and sees the
other two.

Only a player's controls cross the network, never what they do: each machine
sends its own player's buttons, movement and turning every fixed step, and the
host moves that player's character with them. Your own character moves as soon
as you press a key, without waiting for the host.

That is called **prediction**: your machine works out where your character will
be, and the host, which has the final say, checks it. Your machine remembers
what each of its last 64 steps left behind. The host's answer for a step arrives
a little after you took it, and your machine compares it with what it had at
that same step:

- **Close enough**, and your machine's result stands and nothing moves. Close
  enough is within 2 cm, 2 degrees of facing and 0.25 m/s of speed, standing the
  same way (upright or crouched) on the same kind of ground. Small differences
  like rounding never make your view jitter.
- **Further off**, for example because something only the host knew about
  pushed you, your machine goes back to that step. Your character is put where
  the host had it, and any other character you predict where your own machine
  had it then. Then every step since is run again for your characters and their
  cameras, on the controls the host actually received from you. This is called
  a **rollback**. The picture eases across over a tenth of a second instead of
  jumping.

A rollback runs only the engine's own movement again. Your game's systems are
not run again: each runs once per step, when the step happens. The rest of the
world stays where it is, except moving platforms and the physics objects your
machine simulates (see below): each one is put back where it was at every step,
so a character riding a platform or pushing a crate lands where it should.

The tolerances are `predictionTolerance`, `predictionRotationTolerance` (in
radians), `predictionVelocityTolerance` and, for a predicted physics object,
`predictionAngularVelocityTolerance` (in radians a second) in
`assets/config/network.json`. The
editor's network panel counts how many of the host's answers were kept and how
many were rolled back. On a quiet level, almost all of them should be kept.
**Hard snaps** counts answers about a step older than the 64 your machine
remembers: those can't be rolled back to, so your character is put where the
host had it.

Under **Predicted entities**, the panel lists everything your machine
predicts. For each one, it shows its physics and each of its predicted
components separately: how many of the host's answers were compared with it,
how many corrected it, and the last value that differed, yours and the host's.
A correction at a step the host took without your controls, because they
arrived late or not at all, is counted as **starved** instead. The connection
caused that one, not your game code.

### Components you predict

Your machine predicts your character, everything in its blueprint (its camera,
for example), and anything the host hands it with a **`Predicted`** component
naming your client. On those entities, a component marked `predicted` is yours
to change: when the host's older value for it arrives, your machine compares it
with the value it had at that same step instead of writing it over yours.
`Transform` is one, which is why your camera turns the moment you move the
mouse.

Mark a component of your own `predicted` when your player changes it and has to
see the change at once, like ammo or a cooldown:

```cpp
ACOMP(replicable, predicted)
struct Ammo
{
    AFIELD() int32_t rounds = 30;
    AFIELD() float reloadSecondsLeft = 0.f;
};
```

When the host's value disagrees with what your machine had at that step, for
example because the host refused a shot you fired, the host's value is written
over yours. Your ammo can then show the host's older count for a moment, until
the host's answers about your later steps arrive.

A predicted component has to be a plain value: no lists or maps, no
`AFIELD(norep)` field, no entity references, and no member without `AFIELD`.
The build refuses one that breaks a rule. Anything else on your character,
like a score, can stay plain `replicable`: the host's value is simply written
over yours, a little later. Only predicted components are remembered and
compared, so leave the rest plain.

### Physics objects near your player

A crate, a ball or a barrel your character walks up to becomes yours to
simulate. When your character comes within 2 m of a body the physics solver
moves (one with a `RigidBody` that isn't kinematic), the host hands it to your
machine and marks it with a **`SimulatedBy`** component naming your client.
From then on your machine moves it as a single-player game would: it moves the
step you push it, and the host never corrects it. Bodies touching each other
are handed out together, so the crate you push and the one it runs into are
both yours, and stay together.

Every step, your machine sends the host where your bodies ended up, along with
your controls. The host puts its own copy of each one there, so on the host
your character and your crate meet exactly as they did on your machine. Other
players see your crate a moment late, as they see your character.

The host checks what it's told. It takes a body back, and your machine is shown
where the host has it, when your report:

- puts it further from your character than it could be,
- has it moving or spinning faster than a body may,
- or has it further from your last report than that speed could carry it.

It also takes a body back when your reports stop coming for a second and a half,
and when the host's own game pushes or moves it, for example an explosion's
`AddImpulse`: the host applies that itself. Taken back, a body stays with the
host for a moment before it can be handed out again.

You keep a body until your character is more than 4 m from it. Another player
whose character pushes it while yours isn't touching it takes it over after a
moment.

When two or more players push one body at once, no player's machine can
simulate it for the others, so the host takes it over. Each machine whose
character touches it then **predicts** it, as it predicts its own character:
the body moves the step you push it, and when the host's word about that step
arrives, a round trip later, your machine checks its guess and, if it was
wrong, moves the body to where the host has it, easing the difference on
screen. The other players' pushes reach you that late, so while they push too
you will see small corrections. Once only one player has touched the body for
`ownershipTouchDwellTicks`, it is handed to that player again.

To keep a body for the host alone, add **`HostSimulated`** to it. Do it for
something whose every move matters to more than one player, like the ball in a
competitive game. No machine but the host's ever simulates it; yours predicts
it while your character touches it, as above.

The settings are in `assets/config/network.json`:

| Key | What it is |
|---|---|
| `ownershipDistance` | How near your character a body is handed to you (m). |
| `ownershipLeaveDistance` | How far away it goes back (m). Larger than the one above, so a body between two players doesn't flip between them. |
| `ownershipTouchDwellTicks` | How long another player pushes a body yours isn't touching before it is theirs. |
| `ownershipHoldTicks` | How long a body stays with whoever was last given it before someone else can be. |
| `ownershipMaxBodies` | The most bodies one player simulates at once, nearest first. |
| `reportMaxSpeed`, `reportMaxSpin` | The fastest a reported body may move (m/s) and spin (rad/s). |
| `reportReachSlack` | How much further than `ownershipLeaveDistance` a reported body may be from your character (m). |
| `reportTimeoutTicks` | How long the host waits for a report before it takes a body back. |

To try it, open `levels/NetPush.alvl` and run it with **Host + 2 clients**. Each
player has a crate to push. There is one in the middle to fight over, a stack to
knock down, a ball, a crate too heavy to move, and `HostBall`, a ball kept for
the host, to feel the difference.

### Your systems in multiplayer

Your systems run once per step, as in a game with no network: a rollback never
runs them again, so a sound you push from `FixedUpdate` plays once and a value
you count down is counted down once. While a rollback runs, events pushed are
dropped and physics reports no contacts; none of your systems are running then
anyway.

A predicted component works best when your machine can work it out from your
own player alone. Ammo that goes down when your own player fires is fine. A
cooldown that ticks only while your player stands in a zone the host moves will
disagree with the host often, since your machine can't know where the zone is
going: the network panel shows how often the host corrects it.

A system that moves your character itself, say a dash that sets its velocity,
runs once, live. When a rollback runs over the step the dash happened in, the
dash isn't run again, and the host's next answers bring your character back in
line over a moment.

The network panel's **Sync test** checkbox, on a client, checks rollbacks while
you develop: it runs every step a second time and compares the two. Each value
that comes out different is written to the log with both versions. The count
should stay at zero; anything else means the engine's movement doesn't put
something back.

Every machine runs the level's systems, and every machine numbers its steps the
host's way: `ctx.simTick` is the same on every machine for the same step. A
client runs a little ahead of the host, so its step 500 happens slightly before
the host's, but a system that works from the tick alone, like `Oscillate`, puts
things in the same place on both. A client's tick jumps once, when it first
hears from the host.

## Running a dedicated server

The editor binary can also run as a server with no window:

```bash
# A server on port 27015 (the default).
./out/build/gcc-dev/apps/game/Assisi-GameEditor --host 27015 -l levels/NetPile.alvl

# A windowless test client that joins it and logs what it receives.
./out/build/gcc-dev/apps/game/Assisi-GameEditor --connect 127.0.0.1:27015
```

`--connect` takes an IP address, not a host name. `--server` runs the same
headless simulation with no networking at all.

A headless process runs the level's systems in every phase but `Ui`, which is
for screens it doesn't have, and it plays no sounds. It draws nothing, so it
runs one frame for each step, 60 a second by default: `Update` runs as often as
`FixedUpdate` there. A dedicated server hands out characters the same way the editor's host
does, except that the server has none of its own: only the players who join
get one.

Remember the first rule from [Input](input.md): on a server there's no keyboard
or mouse, so `ctx.input` is `nullptr`.
