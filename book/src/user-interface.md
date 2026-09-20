# User interface

In this chapter you'll build a pause menu: Escape opens it, the world stops
behind it, and Resume closes it again.

Menus, HUDs and dialogs are all **screens**, drawn by Mondrian — the engine's
own UI. A screen is a tree of boxes and text that belongs to a world, and it
goes away when that world does.

## Step 1: create the files

Make a new folder `apps/game/src/Tutorial/` with two files in it.

> **Don't worry about what this code means yet.** Copy it as it is. Once you've
> seen it run, the rest of the chapter explains it.

`apps/game/src/Tutorial/TutorialMenu.hpp`:

```cpp
#pragma once

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

/// Gives the world a pause menu when the level loads.
ASYSTEM(Loaded, name = "TutorialMenuScreen")
void TutorialMenuScreenSystem(Assisi::App::SystemContext &ctx);

/// Opens it when Escape is pressed.
ASYSTEM(Update, name = "TutorialMenu", activeWorldOnly)
void TutorialMenuSystem(Assisi::App::SystemContext &ctx);
```

`apps/game/src/Tutorial/TutorialMenu.cpp`:

```cpp
#include "Tutorial/TutorialMenu.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <array>
#include <memory>
#include <string>

using namespace Assisi::Mondrian;

void TutorialMenuScreenSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr)   // a headless server has no UI
    {
        return;
    }

    std::unique_ptr<Screen> screen = std::make_unique<Screen>(
        *ctx.ui,
        ScreenTraits{.input = ScreenInput::ConsumeInput,
                     .beneath = ScreenBeneath::HidesBeneath,
                     .pause = ScreenPause::Pause},
        kSortMenu,
        std::string{"TutorialMenu"});

    // A dark sheet over the whole viewport, with the panel centred on it.
    Style root;
    root.childAlign = {Alignment::Center, Alignment::Center};
    root.background = {0.f, 0.f, 0.f, 0.55f};
    screen->Tree().SetStyle(screen->Root(), root);

    Style panel;
    panel.direction = Direction::Column;
    panel.sizing = {Sizing::Fixed(420.f), Sizing::Fit()};
    panel.padding = Padding::All(32.f);
    panel.gap = 20.f;
    panel.background = {0.10f, 0.11f, 0.14f, 0.94f};
    const NodeId panelId = screen->Add(screen->Root(), panel, "panel");

    Style title;
    title.sizing = {Sizing::Grow(), Sizing::Fit()};
    title.textSize = 48.f;
    screen->AddText(panelId, title, "Paused", "title");

    const ButtonId resume = screen->AddButton(panelId, "Resume");
    screen->OnActivate(resume.node, [](Screen &self) { self.Hide(); });

    ctx.ui->SetFocus(*screen, resume.node);

    // The world takes ownership, and needs "TutorialMenu" to open it.
    const std::array<std::string, 1> needs{"TutorialMenu"};
    Assisi::App::AddScreen(ctx.world, std::move(screen), needs);
}

void TutorialMenuSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr || ctx.input == nullptr)
    {
        return;
    }

    Screen *const menu = Assisi::App::FindScreen(ctx.world, "TutorialMenu");
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

## Step 2: build

The same command as in [Installation](installation.md):

```bash
make gcc-dev
```

## Step 3: turn it on in a level

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

It runs because the screen asked for it:

```cpp
const std::array<std::string, 1> needs{"TutorialMenu"};
Assisi::App::AddScreen(ctx.world, std::move(screen), needs);
```

**A screen declares the systems it needs, and the world installs them.** A level
that shows this menu doesn't have to know it needs `TutorialMenu` to open, and
if you later give the menu a settings page that needs three more systems, no
level file changes.

This is the same bargain a blueprint makes: a level that places a car doesn't
have to know the car needs `Drive`.

## Resume needed nothing

Resume closes the menu, and no system is involved at all:

```cpp
screen->OnActivate(resume.node, [](Screen &self) { self.Hide(); });
```

That's the rule for wiring buttons:

- **Touches only the UI** — closing this screen, opening another — put the
  behaviour on the button, as above. It works wherever the screen is shown,
  with nothing named anywhere.
- **Touches the world** — quitting, loading a level, respawning — push an event
  and let a system answer it. The UI has no access to the world, so this half
  has to go through the queue:

```cpp
screen->OnActivate(quit.node, Assisi::App::QuitRequested{});
```

Read it in a system like any other event (see [Events](events.md)):

```cpp
for (const Assisi::App::QuitRequested &event :
     ctx.events.Read<Assisi::App::QuitRequested>())
{
    // ...
}
```

## The three answers

You gave the screen three answers when you made it:

```cpp
ScreenTraits{.input = ScreenInput::ConsumeInput,
             .beneath = ScreenBeneath::HidesBeneath,
             .pause = ScreenPause::Pause}
```

There's no list of screen types to choose from. You answer three questions, and
any combination is yours.

**`input` — what it does with the pointer and the keys**

| | |
|---|---|
| `NoConsume` | the game reads them as though nothing were shown — a HUD |
| `ConsumeInput` | the screen has them, and Back closes it — your menu |
| `LockedConsumeInput` | the screen has them, and Back does nothing — only your code closes it |

This is why Escape closed the menu without you writing anything: while a
`ConsumeInput` screen is up, the UI has the keys, and Back closes it before the
game ever sees the press. It's also why your `IsKeyPressed(Escape)` doesn't
re-open it immediately — the game doesn't see that press at all.

**`beneath` — whether screens below it are still drawn**

| | |
|---|---|
| `NoHide` | what's below stays visible — a dialog over the game |
| `HidesBeneath` | what's below isn't drawn at all — a full-screen menu |

**`pause` — what happens to the world while it's shown**

| | |
|---|---|
| `Run` | the world keeps simulating |
| `Pause` | the world's fixed step is skipped, and its physics with it |

The UI keeps running either way, which is why the menu that stopped the world
is still clickable.

Change `.pause` to `ScreenPause::Run`, rebuild, and the scene keeps moving
behind the menu. Change `.beneath` to `NoHide` and anything below it stays
drawn.

## Where the screen lives

`AddScreen` gave the screen to the world, and **the world destroys it**. Press
**M** in the demo level to travel to another level: the menu goes with the level
it belonged to, and the new level builds its own.

That's why a HUD doesn't follow you into the main menu, and why nothing has to
remember to clean up.

## Draw order

Screens draw lowest **sort key** first. You passed `kSortMenu`. The engine names
four layers, spaced a thousand apart so you can slot your own between them:

```cpp
kSortHud      // 0     — a HUD, under everything
kSortMenu     // 1000  — a menu
kSortPopup    // 2000  — a dialog over a menu
kSortOverlay  // 3000  — a loading screen, over everything
```

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

`FindScreen` searches the world's screens by name:

```cpp
Screen *const menu = Assisi::App::FindScreen(ctx.world, "TutorialMenu");
```

A system is a plain function with nowhere to keep a pointer between frames, so
looking it up each frame is the normal thing to do — a world has a handful of
screens, and this is a short walk over them.

`Screen::Find`, which looks up a **node** by name, is not the same: it walks
every node on the screen. Use it while building, not every frame.

## What else a screen can hold

Beyond `Add`, `AddText` and `AddButton`:

- `AddToggle` — an on/off switch
- `AddContinuousSlider`, `AddSteppedSlider` — a slider over a range, free or in steps
- `AddTextField` — a box the player types in
- `AddScroll` — a container that scrolls rather than shrinking its content

`Style` carries sizing, direction, padding, gap, alignment, colours, corners,
borders and text size, and is how every node is laid out.

## What's next

You built this menu in C++, which is the engine's foundation. Screen markup,
themes and data bindings arrive on top of this same API — a screen written in
markup behaves exactly like the one you just built by hand.
