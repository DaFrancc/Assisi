# Game settings

Settings your game ships with live in `assets/config/`. There are two kinds:

- **What you decide** goes in `assets/config/`, and ships with the game.
- **What the player changes**, like graphics options and rebound keys, is saved
  in `options.json` next to the game's executable. It overrides your defaults.

## `game.json`

The main settings file:

```json
{
  "version": 1,
  "type": "AppConfig",
  "title": "Assisi Sandbox",
  "startupScene": "levels/Test.alvl",
  "width": 1280,
  "height": 720,
  "physicsHz": 60.0,
  "clearColor": [0.15, 0.15, 0.18, 1.0],
  "keepLogs": 5,
  "keepDumps": 5,
  "simulateFrom": "Begin"
}
```

| Setting | What it does |
|---|---|
| `title` | The window title. Up to 64 characters. |
| `startupScene` | The level the game opens when it starts. **Required**: without it the game refuses to start. |
| `width`, `height` | The window size on first launch. After that, the size the player picked wins. |
| `physicsHz` | How many physics steps run per second. This is also how often `FixedUpdate` systems run. |
| `clearColor` | The background color where nothing is drawn: red, green, blue, alpha, from 0 to 1. |
| `keepLogs` | How many old log files to keep. `0` keeps only the current one. |
| `keepDumps` | How many old crash reports to keep. |
| `simulateFrom` | `"Begin"`: the game starts running as soon as the level is set up, while textures and meshes may still be loading in. `"Loaded"`: the game waits until everything has loaded, which suits a game with a loading screen. |
| `uiScaleMatch` | How big a UI pixel is. `"ShorterSide"` (default): 1/1080 of the screen's shorter side, so the UI reads the same on landscape, portrait, square and ultrawide screens. `"Width"`: 1/1920 of the screen's width. `"Height"`: 1/1080 of the screen's height. See [Lengths](ui-layout.md#lengths). |

Any setting you leave out keeps its default.

## `input.json`

The default key bindings for named actions. See [Input](input.md).

## `network.json`

Multiplayer settings: which components are never sent, and how precisely
positions and velocities are sent. You don't need to touch it for a
single-player game. See [Multiplayer basics](multiplayer.md).

## What the player can change

The player's settings are saved in `options.json`, next to the executable:

- Window size.
- Graphics: anti-aliasing mode, VSync, frame rate limit. The editor's **F11**
  window edits these.
- Rebound keys.

Only settings the player actually changed are stored. Everything else keeps
following your defaults, even when you change them in a later version.

<details>
<summary>Logs and crash reports</summary>

The game writes a log file for each run, named like
`assisi-20260918-132711-44514.log`, next to its executable. Crash reports go in
the same place. `keepLogs` and `keepDumps` control how many old ones are kept.

To store these somewhere else, set the `ASSISI_USER_ROOT` environment variable
to a folder. `options.json` goes there too.

</details>
