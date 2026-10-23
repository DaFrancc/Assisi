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

`AEVENT()` marks the struct as an event. A screen file can then name it in a
button's `on_click` (see [Button actions](ui-actions.md#events)).

## Pushing and reading

Every system reaches events through `ctx.events`.

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

## Each world has its own events

A game can run several worlds at once: the level being left and the one being
loaded, or two levels with players in each. Every world has its own events. An
event pushed in one world is read only by the systems of that world, and the
same goes for a button on a screen the world owns.

This matters because an entity id means nothing outside its world. Two worlds
can each have an entity with the same id, so a `PlaySound` naming entity 12 must
reach entity 12 in the world it came from, not in another.

## Events every world hears

A few events are about the whole program rather than one world, such as
quitting. Those are declared `AEVENT(universal)`, as the engine's
`Assisi::App::QuitRequested` is:

```cpp
AEVENT(universal)
struct QuitRequested
{
};
```

A universal event pushed in any world is read by the systems of every world,
and by the application. You push and read it through `ctx.events` like any
other event; the declaration is what decides who hears it.

Make an event universal only when it doesn't belong to one world. An event
that names an entity is never universal, because the entity is in one world.

## Things to know

- **Don't push an event of the same type you're reading, inside the loop that
  reads it.** Pushing can move the stored events in memory, which breaks the
  loop. Debug builds stop with an error when this happens. Collect what you want
  to push, and push it after the loop.
- **Events aren't saved**, and in multiplayer they aren't sent over the
  network. They only exist inside one frame on one machine.

> **Not yet available:** the engine doesn't raise input events (like "Jump was
> pressed") yet, so input is still read by checking every frame. See
> [Input](input.md).
