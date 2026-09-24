# Button actions

A button's `on_click` says what happens when it is clicked, or when Enter is
pressed while it has focus. It holds either an **action** or an **event**:

- An action is written as a call, with parentheses: `hide()`.
- An event is written as a plain name: `Assisi::App::QuitRequested`.

## Actions

Actions change the UI itself. They need no code and no systems, so a screen
using only actions works in any level.

| Action | Meaning |
|---|---|
| `hide()` | Closes the screen the button is on. |
| `step(target, moves)` | Moves the slider named `target` by `moves` steps. Negative moves go down. |

```xml
<button on_click="hide()">Resume</button>
```

```xml
<button on_click="step(volume, -1)">-</button>
<slider name="volume" min="0" max="100" step="5" value="60" />
<button on_click="step(volume, 1)">+</button>
```

About `step`:

- One move is exactly what one Left or Right arrow-key press does to that
  slider: its `step` on a `slider`, one position on a `stepped_slider`. So
  `step(volume, 2)` equals pressing Right twice.
- The slider reports the change the same way it would for a key press, so
  anything listening with `OnChange` is notified.
- The target must be a `slider` or `stepped_slider` on the same screen. It may
  come before or after the button in the file.
- A disabled or hidden slider isn't moved.

An action must be written with its parentheses: `on_click="hide"` fails the cook.

## Events

Anything that affects the game world — quitting, loading a level, respawning —
goes through an event, because the UI has no access to the world. The button
pushes the event, and a system reads it:

```xml
<button on_click="Assisi::App::QuitRequested">Quit</button>
```

```cpp
for (const Assisi::App::QuitRequested &event :
     ctx.events.Read<Assisi::App::QuitRequested>())
{
    // ...
}
```

See [Events](events.md) for how events work.

To define your own event, mark a struct `AEVENT()` in a header under
`apps/game/src/`:

```cpp
AEVENT()
struct RestartRequested
{
};
```

Refer to it by its full C++ name, including namespaces:

```xml
<button on_click="Game::RestartRequested">Restart</button>
```

Only headers under `apps/game/src/` are scanned for events. An event declared
anywhere else is unknown to the cook, and a button naming it fails the cook.
