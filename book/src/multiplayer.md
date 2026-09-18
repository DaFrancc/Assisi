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
   `RigidBodyDescriptor` are replicable, for example.
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

Remember the first rule from [Input](input.md): on a server there's no keyboard
or mouse, so `ctx.input` is `nullptr`.

<details>
<summary>Building without networking</summary>

If you're making a single-player game, you can leave the networking libraries out
of the build entirely. It makes the first configure noticeably faster.

Use a separate build folder so your normal one isn't affected:

```bash
cmake --preset gcc-debug -DASSISI_ENABLE_NETWORKING=OFF -B out/build/gcc-nonet
cmake --build out/build/gcc-nonet
```

The engine, editor and game all still work. What's missing: the multiplayer
modes in the Game panel, the `Replicated` component and the inspector's
replication controls, and `--host` / `--connect`. Asking for them prints an
error rather than silently doing nothing. `--server` still works, since it isn't
networking.

**A build folder remembers the setting.** Running the preset again without `-D`
doesn't turn networking back on. To check what a folder is set to:

```bash
grep ASSISI_ENABLE_NETWORKING out/build/gcc-nonet/CMakeCache.txt
```

In your code, `#if defined(ASSISI_NETWORKING)` tells you whether networking is
built in, for the rare code that has to know.

</details>
