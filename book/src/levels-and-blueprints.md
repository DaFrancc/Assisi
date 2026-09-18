# Levels and blueprints

## Levels

A **level** (`.alvl`) is a saved world: its entities, their components, and the
list of systems it runs. Levels live in `assets/levels/`, and you build them in
the [editor](editor.md).

The first level the game opens is set in `assets/config/game.json` as
`startupScene`. See [Game settings](game-settings.md).

### Changing level from code

Ask the engine to travel, and it switches at the end of the frame:

```cpp
if (ctx.worldManager != nullptr)
{
    ctx.worldManager->RequestTravel("levels/Level2.alvl");
}
```

The old level is unloaded and the new one becomes the one you see and play.
**Always use `RequestTravel`** from a system; it's safe to call at any point.
If several requests arrive in the same frame, the last one wins.

The example game's `PrettyTravel` system does this when you press **M**.

<details>
<summary>What a level file looks like</summary>

Levels are JSON, so they're readable and work well with git. You don't need to
edit them by hand, but it helps to know the shape:

```json
{
  "entities": [
    {
      "name": "MainCamera",
      "components": {
        "Camera": { "fovDegrees": 60.0, "nearZ": 0.1, "farZ": 500.0, "isActive": true },
        "Transform": {
          "position": [0.0, 3.0, 12.0],
          "rotation": [1.0, 0.0, 0.0, 0.0],
          "scale": [1.0, 1.0, 1.0]
        }
      }
    }
  ],
  "instances": [
    { "name": "Player", "source": "blueprints/Player.abp", "transform": { ... } }
  ],
  "systems": ["CaptureCursor", "CursorToggle"],
  "version": 2
}
```

- `entities` holds each entity's components, by component name.
- `instances` holds the blueprints placed in the level.
- `systems` holds the systems the level runs.
- Rotations are quaternions written `[w, x, y, z]`, so `[1, 0, 0, 0]` means no
  rotation.
- References to assets like meshes and materials are stored by ID
  (`"guid": ...`), with the path alongside only as a hint for humans. That's why
  renaming an asset doesn't break levels, as long as its `.aast` file moves with
  it.

**A level that names a system the game doesn't have won't load**, in the editor
or in the game. It usually means a system was renamed or deleted in code:

- The editor refuses to open it.
- The game refuses to start, if it's the startup level.
- A `RequestTravel` to it fails, and you stay in the current level.

Each time, the log names the missing system. Systems required by blueprints
placed in the level count too. The strictness is deliberate: a level that
loaded anyway would look fine but silently do nothing.

To fix it, add the system back, or open the level in an editor built from the
old code and remove the name in the **Systems** panel. You can also delete the
name from the level file's `systems` list by hand.

</details>

## Blueprints

A **blueprint** (`.abp`) is a reusable group of entities, like a prefab in other
engines: an enemy, a pickup, a door with its frame. Build it once, then place
copies in levels or spawn them while the game runs.

Blueprints live in `assets/blueprints/`, and use the same format as levels. A
blueprint can also list systems it needs. The `Bouncer` blueprint lists the
`Bounce` system, for example, so any level it's placed in runs `Bounce`.

### Making and placing blueprints in the editor

In the **Blueprints** panel:

- **Create from selection** makes a new blueprint from the selected entities.
- **Place instance** puts a copy of the chosen blueprint in front of the camera.

A placed copy is called an **instance**. In the **Entities** panel its entities
are grouped under one row. Clicking the row selects the whole instance.

### Spawning a blueprint from code

```cpp
#include <Assisi/App/BlueprintVerbs.hpp>

Assisi::ECS::Transform where;
where.position = {0.f, 8.f, 0.f};

std::optional<Assisi::ECS::InstanceId> spawned =
    Assisi::App::SpawnBlueprint(ctx.world, "blueprints/Bouncer.abp", where);

if (!spawned)
{
    Assisi::Core::Log::Warn("Could not spawn the bouncer.");
}
```

`SpawnBlueprint` either creates the whole blueprint (entities, meshes and
physics bodies) or nothing at all. It returns the new **instance ID**, or
nothing on failure.

The example game's `BouncerSpawn` system spawns a bouncer every time you press
**F3**.

### Working with a spawned instance

**Keep the instance ID, not the entity handles.** The ID is how you find the
instance again later:

```cpp
using namespace Assisi;

// Find one of its entities by the name it has in the blueprint.
ECS::Entity body = App::FindMember(ctx.world, *spawned, "Body");

// Remove the whole instance.
App::DestroyInstance(ctx.world, *spawned);
```

To customize one copy, spawn it and then change its components as usual. There
are no per-instance overrides.

Instance IDs start from 1 in every level, and start over when a level is loaded.
Don't save one and expect it to mean the same instance in another session.

<details>
<summary>More blueprint functions</summary>

All in `<Assisi/App/BlueprintVerbs.hpp>`:

| Function | What it does |
|---|---|
| `SpawnBlueprint(world, path, transform)` | Creates an instance. |
| `DestroyInstance(world, id)` | Removes an instance and all of its entities. |
| `FindMember(world, id, name)` | One entity of an instance, by name. |
| `FindInstance(world, id)` | The instance's record: which blueprint it came from, and more. |
| `PruneFromInstance(world, entity)` | Detaches one entity from its instance, keeping the entity. |
| `ExplodeInstance(world, id)` | Dissolves the instance, keeping its entities as ordinary ones. |

</details>
