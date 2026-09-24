# Tutorial: a pause menu

You'll build a pause menu that opens when the player presses Escape, stops the
world behind it, and closes when the player clicks **Resume** or presses Escape
again.

## Step 1: Write the screen file

Create `assets/ui/TutorialMenu.amdn`:

```xml
<screen input="consume" beneath="hide" pause="true"
        sort="menu" needs="TutorialMenu"
        align="center center" background="rgbf(0, 0, 0, 0.55)" blocks_pointer="true">

  <column width="420" padding="32" gap="20" background="#1a1c24f0"
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
  [Screen settings](ui-screen-settings.md).
- The screen darkens everything behind it (`background="rgbf(0, 0, 0, 0.55)"`,
  black at 55% opacity) and
  centres its contents (`align="center center"`).
- Inside is a panel (`<column>`) holding a title and a **Resume** button.
- `on_click="hide()"` makes the button close the screen.
- `focus="true"` gives the button keyboard focus when the screen opens, so
  pressing Enter clicks it.

## Step 2: Write the systems that load and open the screen

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
  the Escape key both do that (see [Screen settings](ui-screen-settings.md#input)).

## Step 3: Build

Use the same command as in [Installation](installation.md):

```bash
make gcc-dev
```

## Step 4: Add the screen to a level

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

## Step 5: Try it

Press **F5** to play, then **Escape**. The menu appears and the scene behind it
stops. Click **Resume**, or press Escape again, to close it.

## How the second system was installed

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

To give many levels the same screens, see
[Using a screen in many levels](ui-run-time.md#using-a-screen-in-many-levels).
