# Images

A screen shows a picture in two ways: as an `<image>` element, such as an icon
beside a title, or as an element's **background image**, such as the frame
around a panel. Both come from image files in `assets/`.

| Term | Meaning |
|---|---|
| **Sprite sheet** | A small file naming the pictures cut from one image file. |
| **Sprite** | One named rectangle of a sheet's image, such as an icon. It can be the whole image. |
| **Nine-slice** | A sprite whose corners keep their size while its edges and middle stretch, so one frame fits a box of any size. |

## Sprite sheets

A sprite sheet is a `.amsp` file. This one, `assets/ui/icons.amsp`, cuts two
icons from `ui/icons.png`:

```xml
<sheet texture="ui/icons.png">
  <sprite name="pause" rect="0 0 32 32" />
  <sprite name="contrast" rect="32 0 32 32" />
</sheet>
```

- **`texture`** is the image's path under `assets/`.
- **`rect`** is where the sprite is in the image, in pixels: `x y width
  height`, from the image's top-left corner. Leave it out and the sprite is the
  whole image, so a single picture needs a sheet of one sprite and no
  arithmetic.
- **`borders`** makes the sprite a nine-slice. See [Frames](#frames-nine-slice).

A sheet is named after its file: `ui/icons.amsp` is the sheet `icons`. A screen
names a sprite with the sheet's name, a `:`, and the sprite's name, so
`icons:pause` is the first sprite above. A string table's key is written the
same way.

A `.amsp` file is a sheet only when `assets/config/ui.json` lists it:

```json
{
  "version": 1,
  "type": "UiConfig",
  "stringTables": ["ui/pause.csv"],
  "themes": ["ui/Default.amth"],
  "spriteSheets": ["ui/frames.amsp", "ui/icons.amsp"]
}
```

Two listed sheets with the same file name fail the cook.

## The image element

`<image>` shows one picture, named by a sprite or by an image file's path:

```xml
<image sprite="icons:pause" />
<image texture="ui/logo.png" />
```

An image sized to fit its content, which is what an element is unless its
`width` or `height` says otherwise, is as large as its picture: one pixel of
the picture to one [UI pixel](ui-layout.md#lengths). A 32-pixel icon
is 32 UI pixels across, and grows and shrinks with the rest of the screen.

Given a size of its own, an image fills the space inside its padding as `fit`
says:

| `fit` | What it does |
|---|---|
| `stretch` (default) | Fills the space, changing the picture's shape to match. |
| `contain` | Shows all of the picture, as large as fits, keeping its shape, in the middle of the space. |
| `cover` | Fills the space, keeping the picture's shape, and cuts off what overhangs, as much from each side. |

`tint` multiplies the picture's colours. White, the default, leaves them as
they are, so an icon drawn white takes exactly the tint's colour:

```xml
<image sprite="icons:pause" tint="#e63319" hover:tint="#ffffff" />
```

`corner_radius` rounds an image's corners as it rounds a box's:
`corner_radius="50%"` makes a square picture a circle.

Any element can show a picture with `sprite` or `texture`, drawn under its
text. `<image>` is the element that is a picture and nothing else, and one that
names neither fails the cook.

## Background images

`background_image` draws a sprite across a whole element, over its
`background` colour and under everything inside it:

```xml
<column background_image="frames:panel" padding="32">
  <text>Paused</text>
</column>
```

It takes a sprite only. For a single picture, list a sheet with one sprite and
no `rect`. `background_image` is an attribute about how an element looks, so it
can be given a [state](ui-layout.md#states), such as
`hover:background_image="frames:panel_lit"`, and can be a
[theme token](#frames-from-a-theme). `tint` colours it too.

## Frames (nine-slice)

A sprite with `borders` is drawn as a nine-slice. Its borders cut it into nine
parts: four corners, four edges and a middle.

```xml
<sheet texture="ui/frames.png">
  <sprite name="panel" rect="0 0 48 48" borders="16" />
</sheet>
```

`borders` is one number for every side, or four for left, top, right and
bottom, as `padding` is written, in the sprite's own pixels. When the frame is
drawn:

- **The corners keep their size:** 16 pixels of the sprite are 16 UI pixels on
  screen, whatever size the element is.
- **The edges stretch along their length** and the **middle stretches both
  ways**.
- **An element smaller than its corners** shrinks them evenly to fit, so a
  narrow frame keeps its shape.

A sprite with borders is sliced wherever it is stretched: as a background
image, and as an `<image>` whose `fit` is `stretch`. An image fitted with
`contain` or `cover` keeps its shape, so it is drawn whole.

When drawing a frame, keep each corner's detail inside its border, with a pixel
or two to spare. Whatever lies just inside the border is what the edges and
middle stretch.

## Frames from a theme

A [theme](ui-themes.md) token can hold a sprite, so each mode can give a panel
its own frame:

```xml
<theme>
  <palette mode="dark" frame="frames:panel" />
  <palette mode="high_contrast" frame="frames:panel_contrast" />
  <style name="panel" background_image="$frame" />
</theme>
```

Switching mode then switches every panel's frame. The pause menu in
`assets/ui/Pause.amdn` works this way: its **High contrast** button swaps the
dark frame for a black, white and yellow one.

Icons belong in sheets and screens rather than in themes, so a theme's modes
never have to list them again.

## Image files

UI images are ordinary images under `assets/`, such as `.png` files, cooked
like any other texture. The game loads each listed sheet's image when it
starts, and an image named by its path the first time a screen naming it
loads. Both stay loaded until the game exits.

The colours a UI image draws are the colours in the file, with no lighting or
tone mapping.

> **In the editor, sharp edges can look slightly off.** The editor compresses
> images quickly so it starts fast, and that can put faint specks of one colour
> where two meet, such as a bright line beside a dark one. The cook compresses
> carefully, so the game shows them as drawn. When an image looks wrong in the
> editor, check it in the game before changing the art.

## Images in C++

A screen built in C++ shows a picture with `NodeTree::SetImage`, which takes a
`Sprite`. The UI finds one by name, or loads an image by its path:

```cpp
if (const Assisi::Mondrian::Sprite *icon = ui.FindSprite("icons:pause"))
{
    tree.SetImage(node, *icon);
}
tree.SetImage(logo, ui.TextureSprite("ui/logo.png"));
```

A background image is part of a node's `Style`: set its `backgroundImage` to
the sprite's name, such as `Assisi::Core::InternedString{"frames:panel"}`.

## What the cook checks

The cook fails on a sprite or sheet that doesn't exist, a `rect` reaching past
its image, and `borders` that don't fit inside their sprite: the left and right
borders together must be no wider than the sprite, and the top and bottom no
taller. The full list is in [What the cook checks](ui-cook-checks.md).
