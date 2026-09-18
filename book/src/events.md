# Events

Systems don't call each other. When one system needs to tell others that
something happened, like a player picking up a coin or an enemy dying, it
**pushes an event**. Any system that cares **reads** it later in the same frame.

The sender doesn't know who's listening, and listeners don't know who sent it.
You can add a new reaction (a sound, a score, a particle effect) without
touching the code that raised the event.

## Defining an event

An event is a plain struct. Put it in a header in `apps/game/src/`:

```cpp
#pragma once

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>

AEVENT()
struct CoinCollected
{
    Assisi::ECS::Entity player = Assisi::ECS::NullEntity;
    int32_t value = 0;
};
```

`AEVENT()` marks the struct as an event. It doesn't do anything yet, but keep
it: it's reserved for future features, like sending events over the network.

## Pushing and reading

Every system reaches the event queue through `ctx.events`.

**Push** from the system where the thing happens:

```cpp
ctx.events.Push(CoinCollected{.player = player, .value = 10});
```

**Read** from any system that runs later in the same frame:

```cpp
for (const CoinCollected &coin : ctx.events.Read<CoinCollected>())
{
    Assisi::Core::Log::Info("Picked up {} points", coin.value);
}
```

`Read` gives you every event of that type pushed so far this frame, in the order
they were pushed. If there were none, the loop just doesn't run.

## Events last one frame

At the end of every frame, all events are cleared. So a reader must run
**after** the pusher, in the same frame:

| Pushed in | Can be read in |
|---|---|
| `FixedUpdate` | `Update`, `PostUpdate` |
| `Update` | `PostUpdate`, or a later `Update` system (use `after =`) |
| `PostUpdate` | a later `PostUpdate` system (use `after =`) |

If a reader runs *before* the pusher, it misses the event: by the next frame
it's gone. When two systems in the same phase are involved, order them with
`after =` or `before =` (see [Your first system](first-system.md#options)).

A common pattern is to push in `FixedUpdate` or `Update` and react in
`PostUpdate`, which always runs after both.

## Things to know

- **The queue is shared by every world.** An event pushed in one world can be
  read by systems in another. If that matters, put the world or an entity in the
  event and check it.
- **Don't push an event of the same type you're reading, inside the loop that
  reads it.** Pushing can move the stored events in memory, which breaks the
  loop. Debug builds stop with an error when this happens. Collect what you want
  to push, and push it after the loop.
- **Events aren't saved**, and in multiplayer they aren't sent over the
  network. They only exist inside one frame on one machine.

> **Not yet available:** the engine doesn't raise input events (like "Jump was
> pressed") yet, so input is still read by checking every frame. See
> [Input](input.md).
