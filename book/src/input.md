# Input

Systems read the keyboard and mouse through `ctx.input`, and named actions like
"Jump" through `ctx.actions`.

## The first rule: check for `nullptr`

A game can run **without a window**, for example as a dedicated server, and then
there is no keyboard or mouse. In that case `ctx.input` and `ctx.actions` are
`nullptr`. Every system that reads input starts like this:

```cpp
if (ctx.input == nullptr)
{
    return;
}
```

Also declare input systems `activeWorldOnly`, so only the world the player is
looking at reacts to their keys:

```cpp
ASYSTEM(Update, name = "Jump", activeWorldOnly)
void JumpSystem(Assisi::App::SystemContext &ctx);
```

## Keys and mouse buttons

Include `<Assisi/Window/InputContext.hpp>` and `<Assisi/Window/Key.hpp>`.

```cpp
using Assisi::Window::Key;
using Assisi::Window::MouseButton;

if (ctx.input->IsKeyPressed(Key::Space))   { /* the frame Space went down */ }
if (ctx.input->IsKeyDown(Key::W))          { /* every frame W is held */ }
if (ctx.input->IsKeyReleased(Key::E))      { /* the frame E came up */ }

if (ctx.input->IsMouseButtonPressed(MouseButton::Left))   { /* the frame the button went down */ }
if (ctx.input->IsMouseButtonDown(MouseButton::Right))     { /* every frame Right is held */ }
if (ctx.input->IsMouseButtonReleased(MouseButton::Left))  { /* the frame the button came up */ }
```

The mouse buttons are `MouseButton::Left`, `Right` and `Middle`.

- **Pressed** and **Released** are true for exactly one frame, which suits
  actions like jumping or shooting.
- **Down** is true for as long as it's held, which suits movement.

Read input in an **`Update`** system. A one-frame "pressed" can be missed by a
`FixedUpdate` system, which doesn't run every frame.

### Mouse movement

```cpp
glm::vec2 where  = ctx.input->MousePosition(); // pixels, from the top-left corner
glm::vec2 moved  = ctx.input->MouseDelta();    // movement since last frame
float     scroll = ctx.input->ScrollDelta();   // positive = scrolled up
```

### Capturing the cursor

For first-person controls, hide the cursor and lock it to the window:

```cpp
ctx.input->SetMouseCaptured(true);
```

While captured, `MouseDelta()` keeps reporting movement even at the edge of the
screen. The example game's `CaptureCursor` and `CursorToggle` systems in
`apps/game/src/DemoSystems.cpp` capture the cursor when a level loads, release it
on Escape, and take it back on a click. Add both to a level to get that behavior.

## Named actions

Hard-coding `Key::Space` works, but players expect to rebind keys. **Actions**
fix that: your code asks about `"Jump"`, and a config file says which keys mean
"Jump".

```cpp
#include <Assisi/Window/ActionMap.hpp>

if (ctx.input == nullptr || ctx.actions == nullptr)
{
    return;
}
if (ctx.actions->IsActionPressed("Jump", *ctx.input))
{
    // jump
}
```

There's also `IsActionDown` and `IsActionReleased`. Asking about an action that
doesn't exist returns `false`, so check the spelling if an action never fires.

The default bindings live in `assets/config/input.json`:

```json
{
  "version": 1,
  "type": "InputBindings",
  "actions": {
    "Jump":        ["Space"],
    "MoveForward": ["W", "UpArrow"],
    "Select":      ["LeftMouse"]
  }
}
```

Add your own actions there. When an action lists several keys, any of them
triggers it.

> **Watch the spelling.** In `input.json` the arrow keys are `UpArrow`,
> `DownArrow`, `LeftArrow`, `RightArrow`, and the mouse buttons are
> `LeftMouse` and `RightMouse`. In C++ they're `Key::Up` and
> `MouseButton::Left`.

<details>
<summary>Player rebinding</summary>

A player's own bindings are saved in `options.json`, next to the game's
executable, and override `input.json` one action at a time. Actions the player
hasn't changed keep following `input.json`.

> **Not yet available:** there's no in-game screen for rebinding keys yet.

</details>

<details>
<summary>Every key name</summary>

The full list is the `Key` enum in `modules/Window/include/Assisi/Window/Key.hpp`:
letters `A`–`Z`, digits `Num0`–`Num9`, `F1`–`F12`, `Space`, `Enter`, `Escape`,
`Tab`, `Backspace`, `Insert`, `Delete`, the arrows, the Shift/Control/Alt keys
on both sides, and punctuation.

</details>
