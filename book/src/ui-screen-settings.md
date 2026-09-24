# Screen settings

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

The `<screen>` element is also the root node, so it takes the attributes in
[Layout and appearance](ui-layout.md) (`align`, `background` and so on).

There are no predefined screen types. You combine these settings as you need.
A menu is `input="consume" beneath="hide" pause="true"`; a HUD uses the
defaults; a dialog over live gameplay is `input="consume" beneath="show"
pause="false"`.

## Input

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

## Beneath

| `beneath` | Meaning |
|---|---|
| `show` (default) | Screens below stay visible. Use for a dialog over the game. |
| `hide` | Screens below aren't drawn or laid out. Use for a full-screen menu. |

## Pause

| `pause` | Meaning |
|---|---|
| `false` (default) | The world keeps running. |
| `true` | The world's fixed update (including physics) is skipped while the screen is shown. |

The UI itself always runs, so a screen that pauses the world can still be used.

## Draw order

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
