# Multiple worlds

Most games have one world at a time: the level being played. This chapter is for
when you have more than one loaded, and want your code to deal with that.

## A recap

When a level is loaded, it becomes a **world**, with its own scene (entities and
components), its own physics and its own systems. Several worlds can be loaded
at once, completely separately. An entity belongs to exactly one world, and
objects in two worlds never touch.

Only one world is the **active** one: the world the player sees and controls.

## The same system in several worlds

Each world runs every system its level lists. If two loaded levels both list
`Spin`, it runs once in each, and each run gets its own world in `ctx.world`.

This is why systems should keep their state in components and not in variables:
a `static` or global variable would be shared by the runs in every world.

To make a system run only in the active world, declare it `activeWorldOnly`.
That's what you want for input, the camera and the HUD:

```cpp
ASYSTEM(Update, name = "Hud", activeWorldOnly)
void HudSystem(Assisi::App::SystemContext &ctx);
```

Any system can also check `ctx.isActiveWorld` to see whether it's running in the
active world.

## Looking at other worlds

`ctx.worldManager` gives a system every loaded world. It needs
`<Assisi/App/World.hpp>`:

```cpp
if (ctx.worldManager != nullptr)
{
    ctx.worldManager->ForEach([](Assisi::App::World &world)
    {
        Assisi::Core::Log::Info("World '{}' has {} entities", world.name,
                                world.scene.AliveCount());
    });
}
```

| Call | What it gives you |
|---|---|
| `ctx.worldManager->ForEach(fn)` | Calls `fn(World&)` for every loaded world, oldest first. |
| `ctx.worldManager->Active()` | The active world. |
| `ctx.worldManager->Count()` | How many worlds are loaded. |

`ctx.worldManager` is `nullptr` when there's no world manager, as in some tests, so
check it first.

## The one rule: don't load or destroy worlds from a system

A system runs in the middle of the engine's walk over the worlds. Loading or
destroying one from inside a system could pull the ground out from under it, so
the engine refuses and logs an error if you try.

To change level, use `ctx.worldManager->RequestTravel("levels/Next.alvl")`. It's
applied safely at the end of the frame. See
[Levels and blueprints](levels-and-blueprints.md).

<details>
<summary>Moving an entity to another world</summary>

`WorldManager::MigrateEntity(from, to, entity)` moves an entity, with everything
attached under it, from one world to another, for example to carry the player
through a level change. Its physics body and mesh are rebuilt in the destination
world, and it returns the entity's new handle there. The old handle is no longer
valid.

This is new, and the rules for when it's safe to call are still settling, so
check `modules/App/include/Assisi/App/World.hpp` before relying on it.

</details>
