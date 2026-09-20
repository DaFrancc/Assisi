# User interface

In this chapter you'll build a pause menu: Escape opens it, the world stops
behind it, and Resume closes it again.

Menus, HUDs and dialogs are all **screens**, drawn by Mondrian — the engine's
own UI. A screen is a tree of boxes and text that belongs to a world, and it
goes away when that world does.

You write a screen as a file. The engine compiles it when you cook your
assets, and the game loads the compiled form — so a shipped game never reads
the markup, and a mistake in it stops the build rather than the player.

## Step 1: write the screen

Make `assets/ui/TutorialMenu.amdn`:

```xml
<screen name="TutorialMenu" input="consume" beneath="hide" pause="true"
        sort="menu" needs="TutorialMenu"
        align="center center" background="0 0 0 0.55" blocks_pointer="true">

  <column width="fixed 420" padding="32" gap="20" background="#1a1c24f0"
          corner_radius="16" corner_style="rounded">

    <text width="grow" text_size="48">Paused</text>

    <button name="resume" on_click="hide" focus="true"
            padding="28 10 28 10" text_size="28" background="#e63319"
            corner_radius="10" corner_style="rounded">Resume</button>

  </column>
</screen>
```

Every asset needs a sidecar giving it a stable id. Make
`assets/ui/TutorialMenu.amdn.aast` beside it:

```json
{
  "guid": "1f2e3d4c-5b6a-4798-8899-aabbccddeeff",
  "type": "AssetSidecar",
  "version": 1
}
```

> **Make up your own guid.** Any two assets sharing one is a cook failure. Most
> editors have a "generate UUID" command, or run `uuidgen`.

## Step 2: load it

Make `apps/game/src/Tutorial/TutorialMenu.hpp`:

```cpp
#pragma once

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

/// Gives the world the pause menu when the level loads.
ASYSTEM(Loaded, name = "TutorialMenuScreen")
void TutorialMenuScreenSystem(Assisi::App::SystemContext &ctx);

/// Opens it when Escape is pressed.
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
        Assisi::App::FindScreen(ctx.world, "TutorialMenu");
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

That's the whole of the C++. A screen file cannot know *when* to appear or
which key opens it, and those two things are what's left.

## Step 3: build

The same command as in [Installation](installation.md):

```bash
make gcc-dev
```

## Step 4: turn it on in a level

Open the editor with a level (`-l levels/Test.alvl`), find the **Systems**
panel, type `TutorialMenuScreen` in **Add System**, press **Enter**, and
**Save**.

Add only that one. You'll see why in a moment.

Press **F5** to play, then **Escape**. The menu appears, the scene behind it
stops moving, and clicking **Resume** closes it. Escape closes it too.

If `TutorialMenuScreen` doesn't appear in the Add System box, the build didn't
pick up the file. Check that the header is under `apps/game/src/` and that you
rebuilt.

## You only named one system

The level lists `TutorialMenuScreen`. But Escape is read by `TutorialMenu`,
which you never added.

It runs because the screen asked for it — in the file:

```xml
<screen ... needs="TutorialMenu">
```

**A screen declares the systems it needs, and the world installs them.** A level
that shows this menu doesn't have to know it needs `TutorialMenu` to open, and
if you later give the menu a settings page that needs three more systems, you
add them to `needs` and no level file changes.

This is the same bargain a blueprint makes: a level that places a car doesn't
have to know the car needs `Drive`.

## What a button does

`on_click` takes one of two things.

**A verb** — something the UI does to itself. There is one so far:

```xml
<button on_click="hide">Resume</button>
```

`hide` closes the screen the button is on. It reaches nothing outside the UI, so
it needs no event, no system, and nothing named anywhere. A screen wired this
way works wherever it's shown.

**An event** — for anything that reaches the world: quitting, loading a level,
respawning. The UI has no access to the world, so this half goes through the
event queue:

```xml
<button on_click="Assisi::App::QuitRequested">Quit</button>
```

Read it in a system like any other event (see [Events](events.md)):

```cpp
for (const Assisi::App::QuitRequested &event :
     ctx.events.Read<Assisi::App::QuitRequested>())
{
    // ...
}
```

To make an event of your own, mark a struct `AEVENT()` in a header under
`apps/game/src/`:

```cpp
AEVENT()
struct RestartRequested
{
};
```

and name it by its full C++ name, namespaces included:

```xml
<button on_click="Game::RestartRequested">Restart</button>
```

> **A misspelt name fails the cook**, with the file, line and column. So does an
> event whose header isn't scanned — every header under `apps/game/src/` is, so
> in practice this means you put the struct somewhere else.

## The three answers

You gave the screen three answers at the top of the file:

```xml
<screen input="consume" beneath="hide" pause="true">
```

There's no list of screen types to choose from. You answer three questions, and
any combination is yours.

**`input` — what it does with the pointer and the keys**

| | |
|---|---|
| `none` | the game reads them as though nothing were shown — a HUD |
| `consume` | the screen has them, and Back closes it — your menu |
| `locked` | the screen has them, and Back does nothing — only your code closes it |

This is why Escape closed the menu without you writing anything: while a
`consume` screen is up, the UI has the keys, and Back closes it before the game
ever sees the press. It's also why your `IsKeyPressed(Escape)` doesn't re-open
it immediately — the game doesn't see that press at all.

**`beneath` — whether screens below it are still drawn**

| | |
|---|---|
| `show` | what's below stays visible — a dialog over the game |
| `hide` | what's below isn't drawn at all — a full-screen menu |

**`pause` — what happens to the world while it's shown**

| | |
|---|---|
| `false` | the world keeps simulating |
| `true` | the world's fixed step is skipped, and its physics with it |

The UI keeps running either way, which is why the menu that stopped the world
is still clickable.

Change `pause` to `false`, re-run, and the scene keeps moving behind the menu.
Change `beneath` to `show` and anything below it stays drawn.

## Laying it out

Four elements so far:

| | |
|---|---|
| `column` | children stacked top to bottom |
| `row` | children left to right |
| `text` | words |
| `button` | a button, with its label as its text |

Size is per axis, with `width` and `height`:

```xml
<column width="fixed 420" height="fit">
<column width="grow">
<column width="percent 0.5 min 100 max 800">
```

| | |
|---|---|
| `fit` | just large enough for its content — the default |
| `grow` | its content, then a share of whatever its parent has left |
| `fixed N` | exactly N |
| `percent N` | a fraction of the parent's content size, 0 to 1 |

`min` and `max` clamp any of them.

Beyond that: `padding` (one number for all four edges, or four for left, top,
right, bottom), `gap` between children, `align` (two words, across then down,
each `start`, `center` or `end`), `background`, `border_width`, `border_color`,
`corner_radius`, `corner_style`, `text_size`, `text_color` and `text_align`.

Colours are `#rrggbb`, `#rrggbbaa`, or numbers from 0 to 1:

```xml
background="#1a1c24f0"
background="0 0 0 0.55"
```

All lengths are in logical pixels against a 1920×1080 screen; the engine scales
them to the real one.

## Where the screen lives

`LoadScreen` gave the screen to the world, and **the world destroys it**. Press
**M** in the demo level to travel to another level: the menu goes with the level
it belonged to, and the new level builds its own.

That's why a HUD doesn't follow you into the main menu, and why nothing has to
remember to clean up.

## Draw order

Screens draw lowest **sort key** first. You wrote `sort="menu"`. The engine
names four layers, spaced a thousand apart so you can slot your own between
them:

| | | |
|---|---|---|
| `hud` | 0 | a HUD, under everything |
| `menu` | 1000 | a menu |
| `popup` | 2000 | a dialog over a menu |
| `overlay` | 3000 | a loading screen, over everything |

`sort` takes a number too, so `sort="1500"` sits between a menu and a popup.

Two screens sharing a key draw in the order they were shown.

Screens that consume input also form a **back-stack**. Open a settings screen
over this one and Back returns to the pause menu rather than closing both — and
the pause menu gets back the button that had focus.

## Many levels

You added `TutorialMenuScreen` to one level by hand. With twenty gameplay
levels you don't want to do that twenty times.

Put it in a blueprint that has no entities — nothing but a list of systems.
`assets/blueprints/BaseGameplay.abp`:

```json
{
  "systems": ["TutorialMenuScreen"],
  "entities": [],
  "version": 2
}
```

Place that blueprint in every level that should have a pause menu. A level's
list then says what's particular to that level, and the blueprint says what
kind of level it is. Your main menu is a level too, and leaving the blueprint
out is how it doesn't get a pause menu.

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

The transform is required by the format and means nothing here — the blueprint
places no entities.

</details>

## Finding a screen again

`FindScreen` searches the world's screens by the `name` the file gave:

```cpp
Assisi::Mondrian::Screen *const menu =
    Assisi::App::FindScreen(ctx.world, "TutorialMenu");
```

A system is a plain function with nowhere to keep a pointer between frames, so
looking it up each frame is the normal thing to do — a world has a handful of
screens, and this is a short walk over them.

`Screen::Find`, which looks up a **node** by the `name` you gave it in the file,
is not the same: it walks every node on the screen. Use it while building, not
every frame.

## What's next

Markup is a loader on top of the node API, and adds nothing that API lacks —
anything a file can say, C++ can say by calling `Screen::Add`, `AddText` and
`AddButton` directly. Building a screen by hand is how you'd make one whose
shape isn't known until it's built.

The rest of the controls — toggles, sliders, scrolling containers and text
fields — arrive in the markup next, along with templates for reusing a piece of
a screen, themes for keeping colours out of the layout, and data bindings for
showing what the world holds.
