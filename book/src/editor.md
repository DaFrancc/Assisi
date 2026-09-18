# The editor

The editor, `Assisi-GameEditor`, is your game with editing tools built in. You
build levels in it, and play them without leaving it.

```bash
./out/build/gcc-dev/apps/game/Assisi-GameEditor -l levels/Test.alvl
```

Everything is in panels that you can drag, dock and resize. Your layout is saved
between sessions.

## Moving the camera

| Action | Control |
|---|---|
| Look around | Hold the **right mouse button** and move the mouse |
| Move | **W A S D** or the arrow keys, *while holding the right mouse button* |
| Up / down | **Space** / **Left Ctrl**, while holding the right mouse button |
| Zoom (field of view) | Scroll wheel |
| Fly to an object | Double-click it in the **Entities** panel |

## Selecting and moving objects

- **Left-click** an object to select it. **Ctrl+click** or **Shift+click** adds
  to the selection.
- The **gizmo** appears on the selected object. Drag its handles to change it:

| Key | Gizmo mode |
|---|---|
| **W** | Move |
| **E** | Rotate |
| **R** | Scale |
| **X** | Switch between world and local axes |
| Hold **Ctrl** while dragging | Snap (0.5 m, 15°, or 0.1 scale) |

- **Delete** removes the selection.
- **Ctrl+Z** undoes, and **Ctrl+Y** or **Ctrl+Shift+Z** redoes. The **History**
  panel lists every step.

## The panels

| Panel | What it's for |
|---|---|
| **Entities** | Every object in the level. **+** adds an empty entity, **−** deletes. Objects placed from a blueprint are grouped together. |
| **Inspector** | The selected object's name and components. Edit fields directly, remove a component with its **X**, add one with **Add Component** at the bottom. |
| **Levels** | Open, save and create levels. |
| **Systems** | Which systems this level runs. |
| **Blueprints** | Place a blueprint in the level, or make one from the selection. |
| **Game** | Play, pause and stop. |
| **Gizmo** | Gizmo settings. |
| **Material** | Edit a material. |
| **History** | Undo history. |

## Building a level

### Adding an object

1. Click **+** in the **Entities** panel. You get an empty entity, selected.
2. In the **Inspector**, type `Transform` in the **Add Component** box and press
   **Enter**. The object is placed 5 m in front of the camera.
3. Add a `MeshRenderer` the same way. A cube appears.
4. To use a different mesh, click the **...** button next to the `mesh` field and
   pick one in the asset browser that opens. The `prim://` entries are built-in
   shapes: cubes, spheres, cylinders.
5. Add a `RigidBodyDescriptor` if it should take part in physics.

The **Add Component** box searches as you type, and includes the components you
defined yourself.

### Adding a model

The editor imports **glTF** models (`.gltf` or `.glb`).

1. Copy the model into `assets/`, for example `assets/models/Chair.gltf` along
   with its `.bin` and texture files.
2. Restart the editor, or press **Reimport** in the asset browser. The editor
   gives the model an ID and creates a material file for each of its materials.
3. Pick it with the **...** button on a `MeshRenderer`'s `mesh` field, as above.

> **Not yet available:** only glTF is supported, and textures embedded inside a
> `.glb` aren't read, so keep textures as separate image files (PNG or JPEG).
> There's no drag-and-drop from the file browser into the scene yet.

### Saving

Use the **Levels** panel:

- **Save** writes the level back to its file.
- **Save As**: type a name in the box, then click **Save As**. It's saved as
  `levels/<name>.alvl`. A brand-new level needs this before plain Save works.
- **Load** opens the level picked in the dropdown. **Refresh** rescans the folder
  for new files.

A `*` in the window title means you have unsaved changes.

> There's no **Ctrl+S** yet. Save from the Levels panel.

## Playing

| Key | Action |
|---|---|
| **F5** | Play, or resume when paused |
| **F6** | Pause |
| **F7** or **Esc** | Stop |
| **F8** | While playing, get the mouse cursor back without stopping |

When you stop, the level goes back to exactly how it was before you pressed
play. Changes made while playing aren't kept.

The **Game** panel also lets you play as a multiplayer host, with extra client
windows. See [Multiplayer basics](multiplayer.md).

## Other windows

- **F11** opens **Options**: frame times and graphics settings like shadows,
  anti-aliasing and ambient occlusion.
- **F9** opens the profiler's capture panel. It only works in a `-chiara` build
  (see [Build types](build-types.md)); otherwise it's empty.

<details>
<summary>Command-line options</summary>

Run `Assisi-GameEditor -h` for the full list. The useful ones:

| Option | What it does |
|---|---|
| `-l levels/Name.alvl` | Open this level at startup. |
| `--verbosity info` | Log less. Levels are `trace`, `debug`, `info`, `warn`, `error`, `fatal`. |
| `--server` | Run with no window: just the simulation. |
| `--host [port]` | Run as a multiplayer server. |
| `--connect ip[:port]` | Join a server. |

</details>
