# Screens at run time

## Lifetime

`LoadScreen` gives the screen to a world, and the world destroys it when the
world is destroyed. When the game travels to another level, the old level's
screens are destroyed with it and the new level creates its own. (In the demo
level, press **M** to travel and see this happen.) Screens never need to be
cleaned up by hand.

## Finding a screen and its nodes

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

## Screens stacked on each other

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
