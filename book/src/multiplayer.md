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

The editor's host is a listed, open session called "Assisi editor", so a game
on the same network can find it (see [Browsing](#browsing)).

## Hosting and joining from your game

A **session** is one game that others can join. It has:

- **A name** you choose, and an id the engine makes up when it starts.
- **A visibility.** A *listed* session shows up when players browse the local
  network. A *hidden* one doesn't: players join it by typing its address.
- **An access rule.** *Open* lets anyone in. *Password* lets in whoever types the
  password you chose. *Code* makes up a short code, like `K7MQ4X`, when the
  session starts; players who type it get in, and it stops working when the
  session ends.
- **A most players**, and **the level** being played.

Visibility and access don't depend on each other: a listed session can need a
code, and a hidden one can be open.

Your systems host, join and leave through `ctx.net`. Each call only asks; the
engine does it between frames, and an event says what happened.

### Hosting

```cpp
ctx.net.Host({
    .name       = "Lodge",
    .visibility = Assisi::App::Visibility::Listed, // or Hidden
    .access     = Assisi::App::Access::Code,       // Open, Password or Code
    .password   = "",                              // only read with Access::Password
    .maxPlayers = 4,                               // the host counts
    .port       = 0,                               // 0: the first free port from 27015
});
```

You can leave out any field. Hosting always hosts the level you are playing,
and the machine that hosts plays too. Then one of two events arrives:

- **`SessionStarted`**. `ctx.net.Session()` now has the session's `id`,
  `name`, `port`, `players` and, with `Access::Code`, the `code` to show your
  friends. The code is a plain string; show it as `K7M-Q4X` if you like.
- **`SessionFailed`**, with a `reason`:

| Reason | What it means |
|---|---|
| `PortUnavailable` | Something else is using the port, or no port from 27015 to 27114 was free. |
| `NoLevel` | The level isn't saved in a file, so joiners couldn't load it. |
| `TooMuchData` | The name is over 64 bytes, or the game data is over its limits (below). |
| `BadPassword` | `Access::Password` with no password, or one over 64 bytes. |
| `AlreadyInSession` | This machine is already hosting or joined. Call `Leave()` first. |
| `DirectConnectionsForbidden` | Your `network.json` turns direct connections off (see [Settings](#session-settings)). |

### Joining

```cpp
ctx.net.Join(entry);                           // a session from the browse list
ctx.net.Join(entry, "hunter2");                // one that needs a password or code
ctx.net.Join("192.168.1.20:27015", "K7MQ4X"); // by address; the port defaults to 27015
```

You never say whether the secret is a password or a code: the host knows which.
A code can be typed any way: `k7m q4x`, `K7M-Q4X` and `K7MQ4X` all work.

Joining loads the host's level. Then one of two events arrives:

- **`SessionJoined`**: you are in, and the host's world is arriving.
- **`SessionRefused`**, with a `reason`. Most refusals come before the host's
  level is loaded, and your world stays as it was. Two can come after: a
  `Content` refusal, and a `Level` refusal because the host's level failed to
  load. Then your machine loads the level you were playing again, fresh from
  its file: anything that happened in it since it was loaded is gone.

| Reason | What it means |
|---|---|
| `WrongSecret` | The password or code was wrong or missing. |
| `Full` | The session already has its most players. |
| `TooManyTries` | Too many wrong passwords or codes from this machine. Wait a few seconds and try again; the wait doubles with each wrong try, up to five minutes. |
| `Version` | The host runs a different build of the game. |
| `Content` | The host has different level or blueprint files. |
| `Level` | This machine couldn't load the host's level. The log says why. |
| `NotFound` | The browse entry doesn't name a session. |
| `Unreachable` | Nobody answered at that address. |
| `AlreadyInSession` | This machine is already hosting or joined. Call `Leave()` first. |
| `DirectConnectionsForbidden` | Your `network.json` turns direct connections off. |

### Leaving

`ctx.net.Leave()` ends it. A host ends the session for everyone. A player who
joined goes back to the level they were playing before, with their own
character. Then **`SessionEnded`** arrives with a `reason`:

| Reason | What it means |
|---|---|
| `Left` | This machine called `Leave()`. |
| `HostLeft` | The host ended the session. |
| `ConnectionLost` | The connection to the host dropped. |

Travelling to another level also ends the session; changing level together
isn't built yet.

Each reason has a name for your log or a placeholder menu:
`Assisi::App::Name(refused.reason)` gives `"WrongSecret"`, and so on.

### Browsing

`ctx.net.FindSessions()` asks the local network who is hosting. Every listed
session answers; a hidden one stays quiet. The answers arrive one at a time, so
your list fills in as they come:

- **`SessionFound`**, with an `entry`: a session you didn't know, or one whose
  details changed. Use `entry.id` as the row's key.
- **`SessionLost`**, with an `id`: a session that didn't answer this time.
- **`SessionSearchDone`**: the search is over, two seconds after it started.

Searching again updates the list in place: sessions that answer keep their rows,
and only the ones that stopped answering are lost. That is how a "Refresh"
button should work. `ctx.net.Sessions()` gives the whole list at any time, if
you'd rather read it than keep your own.

Each entry has the session's `name`, `players` and `maxPlayers`, `level`,
`pingMs`, its `gameData`, and `needsSecret`: when it's true, ask the player for
the password or code before calling `Join`. `compatible` is false for a host
running a different build, so you can grey it out.

**Game data** is up to 16 key/value strings you set when hosting, for your own
list to show:

```cpp
ctx.net.Host({.name = "Lodge", .gameData = {{"mode", "coop"}, {"map", "valley"}}});
```

Keys can be 32 bytes long and values 128. The engine never reads them.

A small menu, with the events read in a `Ui` system:

```cpp
void SessionMenuSystem(Assisi::App::SystemContext &ctx)
{
    for (const Assisi::App::SessionFound &found : ctx.events.Read<Assisi::App::SessionFound>())
    {
        ShowRow(found.entry.id, found.entry.name, found.entry.players, found.entry.maxPlayers);
    }
    for (const Assisi::App::SessionLost &lost : ctx.events.Read<Assisi::App::SessionLost>())
    {
        RemoveRow(lost.id);
    }
    for (const Assisi::App::SessionRefused &refused : ctx.events.Read<Assisi::App::SessionRefused>())
    {
        ShowError(Assisi::App::Name(refused.reason));
    }
}
```

`ShowRow`, `RemoveRow` and `ShowError` stand for your own menu code.

### Playing over the internet

Browsing only finds sessions on the local network. Over the internet, a player
joins by address: the host's public IP and port, and the code or password if it
has one. Show the port on the host's screen (`ctx.net.Session()->port`), or host
on a fixed one, so players know what to type. The host's router has to forward
that port. Joining by code alone, without an address, needs an online service
such as Steam, which isn't built yet.

### Session settings

One setting in `assets/config/network.json` is about sessions:

| Key | What it does |
|---|---|
| `allowDirectConnections` | `true` (the default) lets your game host and join by address and find sessions on the local network. `false` turns all of that off: nothing listens for players, `Host` and `Join` refuse with `DirectConnectionsForbidden`, and `FindSessions` finds nothing, raising `SessionSearchDone` straight away. The dedicated server reads the same file. |

Turn it off for a game sold on Steam that must only be played through Steam:
a direct connection skips Steam's check that the player owns the game. Until a
Steam session backend exists, that game has no multiplayer at all.

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

### How far away an entity is sent

By default, every client is sent every `Replicated` entity, wherever it is.
That's fine for a small level. On a big map, turn on the **distance filter** in
`assets/config/network.json`:

```json
"relevancyProvider": "distance",
"relevancyRadius": 60.0,
"relevancyExitRadius": 75.0,
"relevancyDwellTicks": 30
```

Each client is then sent only what is within `relevancyRadius` metres of its
player. Something that moves away is still sent until it is past
`relevancyExitRadius` and has stayed out for `relevancyDwellTicks` fixed steps.
The gap between the two radii stops something on the edge from vanishing and
reappearing over and over.

`Replicated` changes this for one entity:

| Setting | Who is sent it |
|---|---|
| Nothing set | Players within the game's radius. |
| `relevanceRadius` | Players within that many metres, such as 1000 for other players' characters on a big map. It stops being sent the same proportion further out as the game's radii: at 1250 m, with the default 60 and 75. |
| `relevance` set to **Always** | Every player, at any distance: a boss, a round timer, a flare everyone should see. |
| `relevance` set to **ControllerOnly** | Only the player controlling it, such as their inventory. |

An entity attached to another replicated entity, through a `Parent`, goes
wherever its parent goes: a sword in a boss's hand is sent whenever the boss is,
and never on its own, whatever the sword sets. Only **ControllerOnly** still
applies to it.

A player is always sent their own character. A one-off effect, like an
explosion everyone hears, doesn't need an entity at all: send an `independent`
message (see [Messages](#messages)) and let each machine play it.

## How it stays in sync

Every machine runs the same physics simulation, so a client can predict most of
what happens by itself. The host sends only what changed since the last update
each client confirmed, and sends corrections when a client drifts. Corrections
are eased in so they don't look like teleports.

When two machines connect, they first compare a fingerprint of every replicable
component's layout. If the builds differ, they refuse to connect rather than
misreading each other's data. **Host and clients must run the same build.**

They also compare their content: the level the host is running, and every level
and blueprint file each machine has. If any of these differ, even a file neither
machine loads, the client can't join. Copy the host's files and try again.

A packaged game can host and join like the editor can. Its content is checked
against its package, `assets.pak`, so give every player the same package. A
packaged game and the editor never match each other, even with the same files:
the editor checks your source files, and a package holds them cooked. To test a
packaged game online, run the packaged game on every machine.

## Players

Each person in a session controls their own character, and sees everyone
else's. The host hands out the characters:

- **The host keeps the level's own player.** The first character in the level
  marked `Player` (see [Players](players.md)) becomes the host's.
- **Everyone who joins gets a new one**, made at a **`PlayerStart`**. Give an
  entity a `PlayerStart` and a `Transform`, and set its `blueprint` to your
  game's player blueprint, such as the first-person template's
  `blueprints/Player.abp`. There is no default: a start with no blueprint makes
  no character. With several starts, players are spread over them in the order
  they joined. A level with no start gives late joiners nothing, and the log
  says so.
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

### Other players' characters

Your machine moves the other players' characters too, between the host's
updates. With each update, the host sends what every character is being asked
to do: which way it is walking, whether it is sprinting or crouching, and its
`speedScale`. Your machine keeps asking the same until the next update, so the
character walks on at the speed it has on the host instead of slowing down.
The same goes for characters an AI moves on the host.

A jump is not part of this. Your machine sees another player jump when the
host's update says it is in the air. Its `CharacterState` still says what the
host's does, `jumps` and `landings` included: the host sends both counts, and
each changes on your machine once for every jump and landing on the host. That
holds even for a jump that went up and came down between two updates, which
your machine never saw in the air. So an animation that plays a jump when
`jumps` changes plays it for other players too. For a jump like that, `jumps`
and `landings` both change in the same step, so an animation that plays a jump
and then a landing gets both at once. After a long gap in the host's updates, a
count can also go up by more than one in a step. The counts on your machine
aren't the host's numbers, only their changes are the same.

### Players' bodies animate on every machine

The sample game's player has an animated body: a child entity, `Body`, in
`blueprints/Player.abp`, with a skinned model and an `Animator` running
`animation/Player.sgl`. It walks, runs, sprints, crouches, jumps, falls and
lands. The first-person template's `BodyAnimation` system (see
[Players](players.md)) sets the file's params from each player's
`CharacterState`:
- `speed`, its speed across the ground, eased so a correction doesn't jolt it;
- `airborne`, whether it has been off the ground for longer than a step down a
  stair;
- `crouched`, whether it crouches;
- `jump` and `land`, triggers fired when its `jumps` and `landings` counts
  change.

It is the example in
[Setting params from code](animator.md#setting-params-from-code), run for every
player. Landing comes from `landings`, not from the character touching the
ground, which also happens for a moment as it takes off.

Nothing about animation goes over the network. Each machine runs
`BodyAnimation` for every player, its own and the others, from the character
state the host sends. To try it, open `levels/NetPile.alvl`, pick **Host + 2
clients** and press **Run**: walk, sprint (Shift), crouch (Ctrl) and jump
(Space) in one window, and watch that player in the other two. Your own body is
hidden from your own camera, so you see the others'.

The model is one of the test characters that aren't kept in the repository.
Without it, each player is drawn as the [ERROR sign](rendering-models.md#the-error-sign).

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
example because the host refused a shot you fired, your machine **rolls back**,
as it does when the host puts your character somewhere else: it goes back to
that step, takes the host's value, and steps forward again to now. Every
predicted component of the entity is put back to that step too, so whatever
your steps since built on the wrong value is worked out again from the right
one.

Stepping forward again runs only the systems marked `predicted` (see [Your
systems in multiplayer](#your-systems-in-multiplayer)). A predicted component
that a system changes every step must be changed by a predicted system, or the
rollback puts it back and nothing moves it on again.

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

Your systems run once per step, as in a game with no network: a rollback
doesn't run them again, so a sound you push from `FixedUpdate` plays once and a
value you count down is counted down once.

A `FixedUpdate` or `PostFixedUpdate` system that changes predicted state, such
as one that turns your player's controls into movement or counts down a
predicted cooldown, is marked **`predicted`**:

```cpp
ASYSTEM(FixedUpdate, name = "Cooldowns", predicted)
void CooldownSystem(Assisi::App::SystemContext &ctx);
```

A rollback runs it again for every step it goes back over, with your player's
controls as they were at that step, and only on what is being rolled back:
your own character, what is in its blueprint, and what the host has given you
to predict. `ctx.replaying` is true while it does. While a rollback runs, the
events a system pushes are dropped and physics reports no contacts, since they
were all seen the first time the step ran. Mark only the systems that need it:
everything else stays once per step.

After every system of a step has run, the engine clears the turning and the
presses in each player's `PlayerInput`, so every system of the step reads the
same ones in any order and none of yours has to clear them.

A predicted component works best when your machine can work it out from your
own player alone. Ammo that goes down when your own player fires is fine. A
cooldown that ticks only while your player stands in a zone the host moves will
disagree with the host often, since your machine can't know where the zone is
going: the network panel shows how often the host corrects it.

A system that moves your character itself, say a dash that sets its velocity,
should be `predicted` too. Without the flag it runs once, live: when a rollback
runs over the step the dash happened in, the dash isn't run again, and the
host's next answers bring your character back in line over a moment.

What your systems write into a character's `CharacterIntent`, such as a slowing
effect's `speedScale`, is different: a rollback steps each step again with the
intent that step had, so it comes out as it did live.

The network panel's **Sync test** checkbox, on a client, checks rollbacks while
you develop: it runs every step a second time and compares the two. Each value
that comes out different is written to the log with both versions. The count
should stay at zero. Anything else usually means a system that changes
predicted state isn't marked `predicted`, or reads something a rollback doesn't
put back, such as a plain component another player's machine changes.

Every machine runs the level's systems, and every machine numbers its steps the
host's way: `ctx.simTick` is the same on every machine for the same step. A
client runs a little ahead of the host, so its step 500 happens slightly before
the host's, but a system that works from the tick alone, like `Oscillate`, puts
things in the same place on both. A client's tick jumps once, when it first
hears from the host.

## Messages

Replication sends what the world *is*: where things are, what's in each
component. A **message** sends something that *happens* once, like a player
asking to open a door, or the host announcing that a round has started. There
are two kinds, and which way a message goes is part of its declaration:

- An **intent** goes from a player to the host: "I'd like this to happen." The
  host checks it and decides; it may do nothing at all.
- An **event** goes from the host to the players: "this happened." Only the
  host sends events, because the host decides what happens.

### Declaring a message

A message is a struct in a header in `apps/game/src/`, marked `AMSG`, with
`AFIELD` on each member that travels:

```cpp
AMSG(intent, reliable)
struct OpenDoor
{
    AFIELD(controlled) Assisi::ECS::Entity opener; // the character asking
    AFIELD() Assisi::ECS::Entity door;
};

AMSG(event, unreliable)
struct DoorCreaked
{
    AFIELD(subject) Assisi::ECS::Entity door;
};

AMSG(event, reliable, independent)
struct RoundStarted
{
    AFIELD() int32_t round = 0;
};
```

The first word is `intent` or `event`, the second `reliable` or `unreliable`:

- **Reliable** messages always arrive, in the order they were sent. Use it for
  anything a player must not miss.
- **Unreliable** messages are sent once and can be lost. Use it for frequent,
  cosmetic things, where a lost one doesn't matter because another comes soon.

An entity field crosses the network as the same entity on the other machine,
or as `NullEntity` if that machine doesn't have it.

**An event names the entity it's about with `AFIELD(subject)`**, and only the
players who can see that entity are told; one who can't see it yet is told
once it can. An event about no entity in particular is `independent` instead,
and goes to everyone. Each event is one or the other, and the build says so if
it's neither or both.

**An intent marks the entity it acts through with `AFIELD(controlled)`**: the
host refuses the intent unless the sender controls that entity, usually their
own character. Other entity fields are just data: a player can name any door.

### Sending

Every system reaches the network through `ctx.net`:

| Call | Who receives it |
|---|---|
| `ctx.net.SendIntent(intent)` | The host. |
| `ctx.net.Send(event)` | Everyone who can see the event's subject, or everyone for an `independent` event. The host too. |
| `ctx.net.SendToController(entity, event)` | Only the player who controls `entity`, and nobody if no one does. |
| `ctx.net.SendTo(player, event)` | Only that player. |
| `ctx.net.SendExcept(player, event)` | Everyone `Send` would reach, except that player, for example one who already showed the result on their own screen. |

The compiler stops you sending an event with `SendIntent` or an intent with
`Send`. On a player's machine, which isn't the host, the four event calls send
nothing, return `false` and log a warning the first time.

`ctx.net` also tells a system what its machine is:

| Call | Answer |
|---|---|
| `ctx.net.IsAuthority()` | `true` on the host and when playing alone: this machine decides what happens. |
| `ctx.net.IsHost()`, `ctx.net.IsClient()` | Whether this machine is the host, or a player who joined one. |
| `ctx.net.LocalClient()` | Which player this machine is. Compare it with a `ControlledBy` to ask "is this mine?" |

### Reading

A message arrives as an event of type `Received<YourMessage>`, which any system
reads like any other event. Each one holds who sent it and the message:

```cpp
for (const auto &[sender, open] : ctx.events.Read<Received<OpenDoor>>())
{
    // sender is the player who asked; open.door is the door they named.
}
```

`sender` is the player whose connection the message came over, never something
the message claims, so the host can trust it. An event's sender is the host.

A message is read at the start of the step after it arrives: that step reads
it, and so do `Update` and `Ui` that frame, each once. A rollback never reads a
message again. Several systems can react to the same message.

### The checks an intent passes

The host trusts nothing a player sends, so every intent passes these checks
before any system sees it, in this order:

1. **It is an intent.** An event sent by a player is refused.
2. **It isn't part of a flood.** Each player can send each intent type at most 60
   times a second; the rest are dropped.
3. **It is about now.** An intent more than two seconds old, or a second ahead
   of the host, is dropped.
4. **It reads correctly.** A message whose bytes don't decode is dropped.
5. **The sender controls its `controlled` entity.** An honest player can fail
   this for a moment while control of an entity is being handed over.

The network panel counts what each check refused. `AFIELD(min, max)` is not a
check: it limits what can be typed into the inspector. Check that a value makes
sense, such as how far a door is from the player opening it, in the system that
reads it.

The host's own player sends intents too, from the host's machine to itself.
Those can't be flooded or late, so only the last two checks apply, and the
control check is the same for the host as for everyone: a system answering
intents works the same whoever asked.

### Playing alone

A game played alone, with no host and no players, is a host nobody joined:
`ctx.net.IsAuthority()` is `true`, the one player is the host, and what a system
sends reaches its own machine at the next step. An intent arrives from the host,
and an event too, unless it was sent only to some other player, who isn't
there. So the same systems work single-player and online, and you don't write
the game twice.

## Try it: ring a bell for everyone

`levels/NetPile.alvl` has a bell: an entity with an `AudioEmitter` and the
game's own `Bell` component. Open the level, pick **Host + 2 clients** and press
**Run**. Press **B** in any of the three windows: the bell rings in all three,
and the player who rang it plays a gesture in all three, if it is standing or
crouched on the ground.

It takes three systems, in `apps/game/src/Bell.hpp` and `Bell.cpp`. First the
two messages, an intent asking for the bell and an event saying it rang:

```cpp
AMSG(intent, reliable)
struct RingBell
{
    AFIELD(controlled) Assisi::ECS::Entity ringer; // the character asking
    AFIELD() int32_t bell = 0;
};

AMSG(event, reliable, independent)
struct BellRang
{
    AFIELD() Assisi::ECS::Entity ringer;
    AFIELD() int32_t bell = 0;
};
```

They name the bell by number, not as an entity, because the bell isn't
`Replicated`: every machine has its own copy from the level file. They name the
ringer as an entity, because a player is `Replicated`. A machine that doesn't
have the ringer, such as one too far away to be sent it, reads `NullEntity`, and
a level with no player sends `NullEntity` too.

A player asks, naming its own character:

```cpp
void AskForBellSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no keyboard
    {
        return;
    }
    if (!ctx.input->IsKeyPressed(Assisi::Window::Key::B))
    {
        return;
    }
    // Rung by this machine's player, or by nobody in a level with no player.
    Assisi::ECS::Entity ringer = Assisi::ECS::NullEntity;
    for (auto [player, local] : ctx.world.scene.Query<Assisi::App::LocalPlayer>())
    {
        ringer = player;
    }
    ctx.net.SendIntent(RingBell{.ringer = ringer, .bell = kBellOnB}); // 1, the level's bell
}
```

The host answers by telling everyone:

```cpp
void RingBellsSystem(Assisi::App::SystemContext &ctx)
{
    if (!ctx.net.IsAuthority())
    {
        return;
    }
    for (const auto &[sender, ring] : ctx.events.Read<Received<RingBell>>())
    {
        ctx.net.Send(BellRang{.ringer = ring.ringer, .bell = ring.bell});
    }
}
```

And every machine, the host included, plays its bell and the ringer's gesture:

```cpp
void PlayBellsSystem(Assisi::App::SystemContext &ctx)
{
    for (const auto &[sender, rang] : ctx.events.Read<Received<BellRang>>())
    {
        for (auto [entity, bell] : ctx.world.scene.Query<Bell>())
        {
            if (bell.number == rang.bell)
            {
                ctx.events.Push(Assisi::App::PlaySound{.target = entity});
            }
        }
        if (rang.ringer != Assisi::ECS::NullEntity)
        {
            PlayGesture(ctx.world.scene, rang.ringer); // fires its animator's `interact`
        }
    }
}
```

The gesture is an animation trigger, and it doesn't go over the network as one.
The event that rang the bell goes to every machine anyway, so each machine fires
the trigger itself when the event arrives. Do the same with anything the host
decides that should play an animation, such as a hit: send the event, and play
the animation where it's read.

Run the level with **Standalone** and press **B**: the bell still rings,
because playing alone is a host nobody joined.

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

The server's session is listed, so players on the local network find it. Three
more flags set it up:

| Flag | What it does |
|---|---|
| `--name <text>` | The name players see when they browse. "Assisi server" by default. |
| `--max-players <n>` | The most players who can join, 8 by default. The server has no player of its own. |
| `--password <text>` | With `--host`, only players who send this password get in. With `--connect`, the password to send. |

A test client that can't join exits with an error code, so a script can tell.

A headless process runs the level's systems in every phase but `Ui`, which is
for screens it doesn't have, and it plays no sounds. It plays no animation
either: the `Animators` and `AnimationPlayers` systems do nothing there, because
`ctx.animates` is false. It can still load clips and animator files through
`ctx.assets`, for code that needs them on the server. It draws nothing, so it
runs one frame for each step, 60 a second by default: `Update` runs as often as
`FixedUpdate` there. A dedicated server hands out characters the same way the editor's host
does, except that the server has none of its own: only the players who join
get one.

Remember the first rule from [Input](input.md): on a server there's no keyboard
or mouse, so `ctx.input` is `nullptr`.
