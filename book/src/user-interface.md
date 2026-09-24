# User interface

Menus, HUDs and dialogs are drawn by **Mondrian**, the engine's own UI system.
This chapter starts with a tutorial that builds a pause menu, then describes
each part of the system in reference form.

## Overview

These are the terms the rest of the chapter uses:

| Term | Meaning |
|---|---|
| **Screen** | One piece of UI: a menu, a HUD, a dialog. A screen belongs to a world and is destroyed with it. |
| **Node** | One box in a screen. A node can hold text, children, or be a control such as a button. |
| **Screen file** | A `.amdn` file describing one screen, written in an XML-style markup. |
| **Element** | One tag in a screen file, such as `<button>`. Each element becomes one node. |
| **Attribute** | A setting on an element, such as `width="grow"`. |
| **Cooking** | The build step that compiles every screen file into a binary form. |

A screen file is never read by the shipped game. The cook compiles it, and the
game loads the compiled form. Any mistake in a screen file — a misspelt
attribute, an unknown event, a bad value — fails the cook with the file, line
and column, so it is caught at build time rather than by a player.

## Tutorial: a pause menu

You'll build a pause menu that opens when the player presses Escape, stops the
world behind it, and closes when the player clicks **Resume** or presses Escape
again.

### Step 1: Write the screen file

Create `assets/ui/TutorialMenu.amdn`:

```xml
<screen input="consume" beneath="hide" pause="true"
        sort="menu" needs="TutorialMenu"
        align="center center" background="rgbf(0, 0, 0, 0.55)" blocks_pointer="true">

  <column width="fixed 420" padding="32" gap="20" background="#1a1c24f0"
          corner_radius="16" corner_style="rounded">

    <text width="grow" text_size="48">Paused</text>

    <button name="resume" on_click="hide()" focus="true"
            padding="28 10 28 10" text_size="28" background="#e63319"
            corner_radius="10" corner_style="rounded">Resume</button>

  </column>
</screen>
```

What this describes:

- The `<screen>` element is the whole screen. Its attributes say how it
  behaves: it takes the keyboard and mouse (`input="consume"`), hides screens
  below it (`beneath="hide"`), pauses the world (`pause="true"`), and needs a
  system called `TutorialMenu` (`needs`). These are explained in
  [Screen settings](#screen-settings).
- The screen darkens everything behind it (`background="rgbf(0, 0, 0, 0.55)"`,
  black at 55% opacity) and
  centres its contents (`align="center center"`).
- Inside is a panel (`<column>`) holding a title and a **Resume** button.
- `on_click="hide()"` makes the button close the screen.
- `focus="true"` gives the button keyboard focus when the screen opens, so
  pressing Enter clicks it.

### Step 2: Write the systems that load and open the screen

The screen file can't know *when* it should appear or which key opens it. Two
systems handle that:

- `TutorialMenuScreen` runs once when the level loads and creates the screen.
- `TutorialMenu` runs every frame and shows the screen when Escape is pressed.

Create `apps/game/src/Tutorial/TutorialMenu.hpp`:

```cpp
#pragma once

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

/// Creates the pause menu when the level loads.
ASYSTEM(Loaded, name = "TutorialMenuScreen")
void TutorialMenuScreenSystem(Assisi::App::SystemContext &ctx);

/// Shows the pause menu when Escape is pressed.
ASYSTEM(Update, name = "TutorialMenu", activeWorldOnly)
void TutorialMenuSystem(Assisi::App::SystemContext &ctx);
```

and `apps/game/src/Tutorial/TutorialMenu.cpp`:

```cpp
#include "Tutorial/TutorialMenu.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

void TutorialMenuScreenSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr)   // a headless server has no UI
    {
        return;
    }
    Assisi::App::LoadScreen(ctx.world, *ctx.ui, "ui/TutorialMenu.amdn");
}

void TutorialMenuSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr || ctx.input == nullptr)
    {
        return;
    }

    Assisi::Mondrian::Screen *const menu =
        Assisi::App::FindScreen(ctx.world, "ui/TutorialMenu.amdn");
    if (menu == nullptr)
    {
        return;
    }

    if (ctx.input->IsKeyPressed(Assisi::Window::Key::Escape))
    {
        ctx.input->ConsumeKey(Assisi::Window::Key::Escape);
        menu->Show();
    }
}
```

- `LoadScreen` loads the compiled screen and gives it to the world. The screen
  starts hidden.
- `FindScreen` looks the screen up by the path it was loaded from — the same
  string you gave `LoadScreen`.
- `ConsumeKey` marks the Escape press as used, so no other system acts on the
  same press. The key stays hidden from other systems until it is released.
- `Show` makes it visible. Closing it needs no code: the button's `hide()` and
  the Escape key both do that (see [Screen settings](#screen-settings)).

### Step 3: Build

Use the same command as in [Installation](installation.md):

```bash
make gcc-dev
```

### Step 4: Add the screen to a level

1. Open the editor with a level: `-l levels/Test.alvl`.

   When the editor starts, it creates `assets/ui/TutorialMenu.amdn.aast` next
   to your screen file. This sidecar file holds the asset's id; commit it along
   with the screen (see [A tour of the repository](tour.md)).
2. In the **Systems** panel, type `TutorialMenuScreen` in **Add System** and
   press **Enter**.
3. Click **Save**.

Add only `TutorialMenuScreen`. The next section explains why `TutorialMenu`
isn't needed.

If `TutorialMenuScreen` isn't offered in **Add System**, the build didn't find
the header. Check that it is under `apps/game/src/` and rebuild.

### Step 5: Try it

Press **F5** to play, then **Escape**. The menu appears and the scene behind it
stops. Click **Resume**, or press Escape again, to close it.

### How the second system was installed

The level only lists `TutorialMenuScreen`, yet `TutorialMenu` — the system that
reads Escape — is running. The screen file asked for it:

```xml
<screen ... needs="TutorialMenu">
```

**A screen lists the systems it needs in `needs`, and the world installs them
when the screen is loaded.** The level doesn't have to know what the menu
requires. If the menu later needs more systems, you add them to `needs` and no
level file changes. Blueprints work the same way: a level that places a car
doesn't list the systems the car needs.

## Screen settings

The attributes on the `<screen>` element describe the screen as a whole:

| Attribute | Values | Meaning |
|---|---|---|
| `input` | `none`, `consume`, `locked` | What the screen does with the keyboard and mouse. |
| `beneath` | `show`, `hide` | Whether screens below this one are still drawn. |
| `pause` | `true`, `false` | Whether the world stops while the screen is shown. |
| `sort` | `hud`, `menu`, `popup`, `overlay`, or a number | Where the screen draws relative to others. |
| `needs` | system names, separated by spaces | Systems the world installs when the screen is loaded. |

A screen has no `name` attribute. It is identified by the path of its file,
such as `ui/Pause.amdn`, so renaming or moving the file is all it takes to
rename the screen. Writing `name` on `<screen>` fails the cook.

The `<screen>` element is also the root node, so it takes the layout and
appearance attributes described later (`align`, `background` and so on).

There are no predefined screen types. You combine these settings as you need.
A menu is `input="consume" beneath="hide" pause="true"`; a HUD uses the
defaults; a dialog over live gameplay is `input="consume" beneath="show"
pause="false"`.

### Input

| `input` | Keyboard and mouse | Back (Escape) |
|---|---|---|
| `none` (default) | The game receives them as if the screen weren't there. Use for a HUD. | Does nothing. |
| `consume` | The screen receives them; the game doesn't. | Closes the screen. |
| `locked` | The screen receives them; the game doesn't. | Does nothing. Only your code can close the screen. |

Only the topmost shown screen that consumes input receives it. This is why
Escape closed the tutorial menu without any code: while a `consume` screen is
shown, the UI handles Escape as Back and closes the screen before the game sees
the key. It is also why `IsKeyPressed(Escape)` in `TutorialMenu` doesn't
immediately reopen the menu — the game never sees that key press.

### Beneath

| `beneath` | Meaning |
|---|---|
| `show` (default) | Screens below stay visible. Use for a dialog over the game. |
| `hide` | Screens below aren't drawn or laid out. Use for a full-screen menu. |

### Pause

| `pause` | Meaning |
|---|---|
| `false` (default) | The world keeps running. |
| `true` | The world's fixed update (including physics) is skipped while the screen is shown. |

The UI itself always runs, so a screen that pauses the world can still be used.

### Draw order

Screens are drawn in order of their **sort key**, lowest first. The engine
names four layers, a thousand apart so you can place your own between them:

| `sort` | Key | Typical use |
|---|---|---|
| `hud` | 0 | A HUD, below everything |
| `menu` (default) | 1000 | A menu |
| `popup` | 2000 | A dialog over a menu |
| `overlay` | 3000 | A loading screen, above everything |

`sort` also accepts a number: `sort="1500"` draws between menus and popups.
Screens with the same key draw in the order they were shown.

## Elements

Each element becomes one node. There are nine:

| Element | What it is | Holds text | Holds elements |
|---|---|---|---|
| `column` | A container that stacks its children top to bottom | No | Yes |
| `row` | A container that places its children left to right | No | Yes |
| `text` | Text | Yes | No |
| `button` | A button; its text is its label | Yes | No |
| `toggle` | An on/off switch | No | No |
| `slider` | A slider that can rest anywhere between its ends | No | No |
| `stepped_slider` | A slider that rests only on fixed positions | No | No |
| `scroll` | A container that scrolls its children instead of shrinking them | No | Yes |
| `text_field` | A box the player types into; its text is what it starts with | Yes | No |

Text goes between the tags: `<text>Paused</text>`. Text inside an element that
doesn't hold text, such as a `<row>`, fails the cook. So does an element inside
one that doesn't hold elements.

Any element can also have a `name` (see [Names](#names)) and `focus="true"` to
take keyboard focus when the screen opens. Only one element per screen may have
`focus="true"`.

## Layout and appearance

These attributes work on every element, including `<screen>` and the controls.

All lengths are in logical pixels on a 1920×1080 screen. The engine scales them
to the actual window size.

### Size

`width` and `height` each take one of four sizing modes:

| Mode | Meaning |
|---|---|
| `fit` (default) | Just large enough for the content. |
| `grow` | Large enough for the content, plus a share of the space the parent has left over. |
| `fixed N` | Exactly N. |
| `percent N` | A fraction (0 to 1) of the parent's content area. |

Any mode can be followed by `min N` and `max N` to limit it:

```xml
<column width="fixed 420" height="fit">
<column width="grow">
<column width="percent 0.5 min 100 max 800">
```

### Arranging children

| Attribute | Values | Meaning |
|---|---|---|
| `direction` | `row`, `column` | How children are arranged. `<row>` and `<column>` set this already; `<screen>` is a column. |
| `padding` | `N`, or `left top right bottom` | Space between the element's edge and its children. |
| `gap` | `N` | Space between children. |
| `align` | two words: horizontal, then vertical | Where children sit. Each word is `start`, `center` or `end`. |

### Colours, borders and corners

| Attribute | Values | Meaning |
|---|---|---|
| `background` | colour | Fill colour. |
| `border_width` | `N` | Border thickness. |
| `border_color` | colour | Border colour. |
| `corner_radius` | `N` | Size of the corners. |
| `corner_style` | `square`, `rounded`, `cut` | Corner shape. |

A colour is written in one of three forms. Each says which scale its numbers
are on:

| Form | Channels | Example |
|---|---|---|
| `#rrggbb`, `#rrggbbaa` | Hex digits, `00` to `ff` | `#e63319`, `#1a1c24f0` |
| `rgb(r, g, b)`, `rgb(r, g, b, a)` | Whole numbers from 0 to 255 | `rgb(230, 51, 25)` |
| `rgbf(r, g, b)`, `rgbf(r, g, b, a)` | Numbers from 0 to 1 | `rgbf(0, 0, 0, 0.55)` |

The fourth channel is alpha (opacity), on the same scale as the others. Without
it, the colour is fully opaque.

Bare numbers such as `background="1 1 1"` fail the cook, because they don't say
which scale they're on: `1 1 1` is white on a 0–1 scale and nearly black on a
0–255 scale. A channel outside its call's range also fails, with a message
naming the call that takes that range.

### Text

| Attribute | Values | Meaning |
|---|---|---|
| `text_size` | `N` | Font size. |
| `text_color` | colour | Text colour. |
| `text_align` | `left`, `center`, `right` | Horizontal alignment of the text. |

### Floating

A floating element is placed on top of the others instead of taking space in its
parent — a badge on a corner, for example.

| Attribute | Values | Meaning |
|---|---|---|
| `float` | `true`, `false` | Whether the element floats. |
| `float_target` | `parent`, `root` | What it is placed against: its parent, or the whole screen. |
| `float_anchor` | two alignment words | The point on the target to attach to. |
| `float_attach` | two alignment words | The point on the element that goes on the anchor. |
| `float_offset` | `x y` | A further shift after attaching. |
| `float_clip` | `true`, `false` | Whether the parent's edges clip it. |

### Scrolling

These set how a `<scroll>` element (see [Scroll](#scroll)) looks and moves:

| Attribute | Values | Meaning |
|---|---|---|
| `scroll_bar_visibility` | `never`, `when-needed`, `always` | When the scroll bar is shown. `when-needed` shows it only while some content is out of view. |
| `scroll_bar_drag` | `follows-pointer`, `smoothed` | Whether content moves with the dragged bar exactly, or glides after it. |
| `scroll_smoothing` | seconds | How long scrolling takes to reach its destination. `0` jumps immediately. |
| `scroll_bar_min_length` | `N` | The shortest the scroll bar's handle can be. |

### Behaviour

| Attribute | Values | Meaning |
|---|---|---|
| `visible` | `true`, `false` | Whether the element is shown. A hidden element takes no space. |
| `enabled` | `true`, `false` | Whether a control responds to the player. |
| `blocks_pointer` | `true`, `false` | Whether clicks on this element stop here instead of reaching the game. Use on a panel's background. |
| `takes_keyboard` | `true`, `false` | Whether, while focused, it receives the keyboard even when the game would otherwise have it. |
| `selectable` | `true`, `false` | Whether the player can select and copy its text. |
| `style` | a name | Reserved for themes. Accepted and stored, but has no effect yet. |

## Controls

Controls take all the attributes above. This section lists the extra attributes
each control is created with.

### Toggle

```xml
<toggle name="fullscreen" on="true" />
```

| Attribute | Meaning |
|---|---|
| `on` | Whether it starts on. |

### Slider and stepped slider

```xml
<slider name="volume" min="0" max="100" step="5" value="60" />
<stepped_slider name="quality" min="0" max="3" steps="4" value="1" />
```

| Attribute | `slider` | `stepped_slider` |
|---|---|---|
| `min`, `max` | The values at each end. | The values at each end. |
| `step` | How far one arrow-key press moves it. **Required**, and cannot be 0. | Not accepted. |
| `steps` | Not accepted. | How many positions it has. |
| `value` | The starting value. | The starting position, counted from 0. |

`slider` requires `step` because no default suits every range: a step of 0.1 is
reasonable on a 0–1 slider but would take a thousand presses on a 0–100 one.

A `stepped_slider` always moves one position per press, so it has no `step`.

### Scroll

```xml
<scroll name="list" axes="y" height="fixed 150" scroll_bar_visibility="when-needed">
  <button>One</button>
  <button>Two</button>
</scroll>
```

| Attribute | Values | Meaning |
|---|---|---|
| `axes` | `x`, `y`, `xy`, `none` | Which directions it scrolls in. |

Other elements write this setting as `scroll_bars`. On a `<scroll>`, only
`axes` is accepted, so the setting can't be written twice.

### Text field

```xml
<text_field name="player" lines="single" placeholder="your name" max_length="24" />
<text_field name="secret" lines="single" mask="dots" />
<text_field name="notes" lines="multi up-to 3" />
```

| Attribute | Values | Meaning |
|---|---|---|
| `lines` | `single`, `multi`, `multi up-to N`, `multi exactly N` | How many lines it holds, and how tall it is in lines. |
| `placeholder` | text | Shown in fainter text while the field is empty. |
| `mask` | `none`, `dots` | `dots` hides what is typed, for passwords. |
| `max_length` | `N` | The most characters the player can type. |
| `pattern` | a pattern name, or `/expression/` | What the text must look like. See [Patterns](#patterns). |
| `check` | `refuse`, `on-change`, `on-commit` | When the pattern is checked. See [Patterns](#patterns). |

The text between the tags is what the field starts with:

```xml
<text_field name="player">type here</text_field>
```

`lines` sets the field's height in lines of text. `height` still sets the size of
its box, as on any element. A field limited to a number of lines refuses input
that would go past the limit rather than scrolling; put it inside a `<scroll>` to
hold more than it shows.

### Patterns

`pattern` takes either the name of a built-in pattern or a regular expression
between slashes:

```xml
<text_field name="port" pattern="integer" check="refuse" />
<text_field name="who" pattern="/[^@ ]+@[^@ ]+/" check="on-commit" />
```

The built-in patterns are `alphabetic`, `alphanumeric`, `integer`, `real` and
`email`. A value without slashes must be one of these names, or the cook fails.
This stops a misspelt name such as `emial` from being read as an expression that
matches only those letters.

An expression that isn't valid also fails the cook.

`check` says when the pattern is applied:

| `check` | Meaning |
|---|---|
| `refuse` | Typing that could never match is rejected as it is typed. |
| `on-change` | The text is checked after every change. |
| `on-commit` (default) | The text is checked when the player presses Enter or leaves the field. |

Use `refuse` for patterns that describe each character, such as digits. Don't
use it for patterns that describe a finished value, such as an email address:
`jim@` doesn't match yet, and refusing it would stop anyone typing an address.

## Names

A `name` identifies a node so that code and other elements can refer to it:

- `Screen::Find` looks a node up by name from C++.
- A button's `step(...)` action names the slider it moves.
- A template instance's name is added to the names inside it.

The rules:

- **Names are optional.** Name the nodes you need to refer to and leave the rest
  unnamed.
- **A name must be unique on its screen.** Two nodes with the same name fail the
  cook, and the error gives both lines.
- **A name can't contain `.`.** The dot is used for names inside template
  instances (see [Templates](#templates)).
- **A button's label is not its name.** Two buttons can both read "Back"; you
  find each by its `name`.

## Button actions

A button's `on_click` says what happens when it is clicked, or when Enter is
pressed while it has focus. It holds either an **action** or an **event**:

- An action is written as a call, with parentheses: `hide()`.
- An event is written as a plain name: `Assisi::App::QuitRequested`.

### Actions

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

### Events

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

## Templates

A template lets you define an element once and reuse it. Use templates when
several parts of a screen share a look or a structure.

### Declaring and using a template

A template is declared directly inside `<screen>`. It has a name and one **root
element**, which can hold any number of elements inside it (see the
`labelled_slider` example below). Here the root is a single button:

```xml
<template name="menu_button">
  <button padding="28 10 28 10" text_size="28"
          corner_radius="10" corner_style="rounded" />
</template>
```

The template's name can then be used as an element anywhere on that screen,
before or after the declaration. Each use is called an **instance**:

```xml
<menu_button name="resume" on_click="hide()" background="#e63319">Resume</menu_button>
<menu_button name="quit" on_click="Assisi::App::QuitRequested"
             border_width="2" border_color="#ffffff">Quit</menu_button>
```

An instance is a copy of the template's element, changed by the instance:

| On the instance | Effect |
|---|---|
| Attributes | Replace the template's attribute of the same name. Attributes the instance doesn't set come from the template. |
| Text | Replaces the template's text, if the instance has any. |
| Child elements | Are added after the template's children. |

The cook replaces every instance with ordinary elements. The compiled screen is
identical to one written out by hand, so nothing at run time knows templates were
used.

### Names inside a template

Nodes inside a template can have names, and actions inside it can refer to them:

```xml
<template name="labelled_slider">
  <row>
    <text name="label" />
    <button name="down" on_click="step(slider, -1)">-</button>
    <slider name="slider" min="0" max="100" step="5" />
  </row>
</template>

<labelled_slider name="music" />
<labelled_slider name="effects" />
```

Each instance adds its own name and a dot in front of the names inside it. This
example creates `music.label`, `music.down`, `music.slider`, `effects.label`,
and so on.

#### Which node an action's target means

An action's target is looked up where the action is written:

- **Written inside the `<template>`**, a target means a node in the same
  instance. The template's `step(slider, -1)` becomes `step(music.slider, -1)`
  in the `music` instance and `step(effects.slider, -1)` in the `effects`
  instance, so each `-` button moves its own slider.
- **Written anywhere else in the screen**, a target means a node on the screen,
  and a node inside an instance needs its full name. This includes attributes
  on the instance element itself, because they are written in the screen, not
  in the template.

```xml
<screen>
  <labelled_slider name="music" />

  <!-- Written in the screen: the full name is needed. -->
  <button on_click="step(music.slider, 1)">+</button>   <!-- moves music.slider -->
  <button on_click="step(slider, 1)">+</button>         <!-- refused: the screen has no node called "slider" -->
</screen>
```

An instance without a name leaves the names inside unchanged. That works once;
a second unnamed instance would create the same names again, so the cook asks
you to name it. A template with no names inside can be used without a name any
number of times.

### Template rules

- A template is declared directly inside `<screen>`, nowhere else.
- Its name can't be an existing element name such as `button`, and two templates
  can't share a name.
- It has exactly one root element and no text of its own. The root can hold
  any number of elements. There is one root because an instance becomes one
  node, and the instance's attributes, text and children apply to that root.
  To group several elements, wrap them in a `<row>` or `<column>`.
- That element must be a built-in element. It can contain instances of other
  templates, but can't itself be one.
- A template can't contain itself, directly or through other templates.
- Templates can be nested up to eight deep.
- Every template is checked by the cook even if nothing uses it. An error inside
  one is reported at its line; if the error only appears in a particular
  instance, the message also gives the instance's line.

## Screens at run time

### Lifetime

`LoadScreen` gives the screen to a world, and the world destroys it when the
world is destroyed. When the game travels to another level, the old level's
screens are destroyed with it and the new level creates its own. (In the demo
level, press **M** to travel and see this happen.) Screens never need to be
cleaned up by hand.

### Finding a screen and its nodes

`FindScreen` returns a world's screen by the path it was loaded from:

```cpp
Assisi::Mondrian::Screen *const menu =
    Assisi::App::FindScreen(ctx.world, "ui/TutorialMenu.amdn");
```

A screen built in C++ has no file, so it is found by the name given to its
constructor instead.

Systems can't keep pointers between frames, so calling `FindScreen` every frame
is normal. A world has only a few screens, so the lookup is cheap.

`Screen::Find` returns a node by its name. It searches every node on the
screen, so call it once while setting up and keep the result rather than
calling it every frame.

### Screens stacked on each other

Screens that consume input form a **back-stack**. If a settings screen is opened
over the pause menu, Back closes only the settings screen and returns to the
pause menu, and the pause menu's focus returns to the button that had it.

## Using a screen in many levels

In the tutorial you added `TutorialMenuScreen` to one level by hand. To give
many levels the same screens, put the systems in a blueprint that has no
entities, only a list of systems. Create `assets/blueprints/BaseGameplay.abp`:

```json
{
  "systems": ["TutorialMenuScreen"],
  "entities": [],
  "version": 2
}
```

Place this blueprint in every level that should have the pause menu. A level
that shouldn't, such as a main menu, simply doesn't include it.

<details>
<summary>What the level file looks like</summary>

A placed blueprint is an entry in the level's `instances` array:

```json
{
  "instances": [
    {
      "name": "BaseGameplay",
      "source": "blueprints/BaseGameplay.abp",
      "transform": { "position": [0, 0, 0], "rotation": [1, 0, 0, 0], "scale": [1, 1, 1] }
    }
  ],
  "systems": [],
  ...
}
```

The transform is required by the format but has no effect here, because the
blueprint places no entities.

</details>

## Building a screen in C++

Screens can also be written in C++, with `Screen::Add`, `AddText`, `AddButton`
and the control functions such as `AddContinuousSlider`.

Screen files are the standard way to write a screen. They can describe
everything the C++ functions can — loading a screen file calls those same
functions — and they are easier to read.

## What the cook checks

Every one of these fails the cook with the file, line and column:

- An unknown element, attribute or value.
- Text or child elements inside an element that can't hold them.
- A `slider` without a `step`, or with `step="0"`.
- A pattern name that doesn't exist, or an expression that isn't valid.
- Two nodes with the same name, or a name containing `.`.
- More than one element with `focus="true"`.
- An `on_click` on anything other than a button.
- An event that no header under `apps/game/src/` declares.
- An action that doesn't exist, has the wrong number of arguments, or is written
  without parentheses.
- A `step` target that doesn't exist on the screen or isn't a slider, or a
  `step` of 0 moves.
- A template that is declared in the wrong place, is badly formed, contains
  itself, or nests more than eight deep.
- An unnamed instance that would repeat names made by another.

## What's next

Themes, for keeping colours and sizes out of the layout, and data bindings, for
showing values from the world, are planned next.
