# Themes

A **theme** keeps a game's colours, sizes and spacing in one file, out of its
screens. A screen names a look, such as "the primary button", and the theme
says what that look is. Changing the theme changes every screen at once, with
no screen file edited, and the game can switch themes while it runs: for a
high-contrast mode, a dark mode, or a different look per faction.

A theme file has two parts:

| Term | Meaning |
|---|---|
| **Style** | A named set of attributes, such as `padding` and `background`, that an element takes by naming it. |
| **Token** | A named value, such as a colour or a length, that a style or an element uses with `$name`. |
| **Palette** | The values every token has in one **mode**. A theme has one palette per mode, so switching mode changes every token's value at once. |

## A theme file

A theme is a `.amth` file. This one has two modes, `dark` and
`high_contrast`, and two styles:

```xml
<theme>
  <palette mode="dark" surface="#1a1c24f0" edge="#575f7a" edge_width="2"
           accent="#e63319" accent_hover="#ff6a4d" />
  <palette mode="high_contrast" surface="#000000" edge="#ffffff" edge_width="4"
           accent="#ffd23c" accent_hover="#ffe680" />

  <style name="panel" background="$surface" border_width="$edge_width"
         border_color="$edge" corner_radius="16" corner_style="rounded" />
  <style name="primary_button" padding="28 10 28 10" text_size="28"
         background="$accent" hover:background="$accent_hover" />
</theme>
```

- **A palette's attributes are its tokens.** `mode` names the mode; every
  other attribute gives a token its value in that mode. Every palette gives
  every token a value.
- **A token holds any value an attribute can.** A colour, a length such as
  `edge_width`, a number, or a word such as `rounded`.
- **A style takes the attributes an element takes about how it looks:** sizes,
  spacing, colours, borders, text, floating and scrolling, written as they are
  on an element, with a [state](ui-layout.md#states) in front if you like. It
  can't say what a node is or does: `on_click`, `enabled`, `visible`, `name`
  and the like stay on the element.

## Using a style

An element names one style with `style`:

```xml
<column style="panel" width="420" padding="32">
  <button style="primary_button" on_click="Game::ResumeRequested">Resume</button>
</column>
```

What the element writes itself wins over its style. The panel above takes its
background, border and corners from `panel`, and its width and padding from
its own attributes. In full, weakest first:

1. The style's attributes.
2. The element's own attributes.
3. The style's attributes for a state that holds, such as `hover:background`.
4. The element's own attributes for that state.

So a style's hover look shows over an element's own background, and an
element's own `hover:background` wins over both. When several states hold, they
still stack as [States](ui-layout.md#states) describes: a disabled look always
shows.

`row` and `column` count as writing `direction` themselves, so a style can't
turn a column into a row.

## Using a token

Write `$` and a token's name anywhere an element or a style takes a value for
how it looks:

```xml
<screen background="$scrim">
  <text text_color="$accent">Paused</text>
</screen>
```

A token can't stand for what an element is or does, or for its text: words
come from [string tables](ui-string-tables.md).

## Building a style on another

`extends` starts a style from another and changes only what it writes:

```xml
<style name="menu_button" padding="28 10 28 10" text_size="28" corner_radius="10" />
<style name="primary_button" extends="menu_button" background="$accent" />
<style name="secondary_button" extends="menu_button" border_width="2" border_color="$edge" />
```

The base can be written anywhere in the file, before or after the style that
extends it.

## Listing themes

A `.amth` file is a theme only when `assets/config/ui.json` lists it:

```json
{
  "version": 1,
  "type": "UiConfig",
  "stringTables": ["ui/pause.csv"],
  "themes": ["ui/Default.amth", "ui/Faction.amth"]
}
```

The game starts in the first theme listed, in its first mode.

Every screen is checked against every theme listed, in every mode, because the
game might be wearing any of them. A style one theme doesn't have, or a token
one mode gives a value its attribute can't hold, fails the cook. A screen that
names a style or a token while `ui.json` lists no theme fails too.

## Switching while the game runs

Game code switches a theme by the path it was listed under, and a mode by its
name:

```cpp
ctx.ui->SetTheme("ui/Faction.amth");
ctx.ui->SetThemeMode("high_contrast");
```

Every screen changes at once, including screens loaded later. `SetTheme` keeps
the current mode when the new theme has one with the same name, and uses its
first mode otherwise, so a player who chose high contrast keeps it. Both
return an error, log why and change nothing when the theme or mode doesn't
exist. `GetTheme` and `GetThemeMode` say which are worn now.

The pause menu in `assets/ui/Pause.amdn` has a **High contrast** button, which
pushes the event `ContrastToggled`. `PauseMenuSystem`, in
`apps/game/src/PauseMenu.cpp`, answers it by switching the mode:

```cpp
for ([[maybe_unused]] const ContrastToggled &toggled : ctx.events.Read<ContrastToggled>())
{
    (void)ctx.ui->SetThemeMode(ctx.ui->GetThemeMode() == kContrastMode ? kEverydayMode : kContrastMode);
}
```

## Styles in C++

A screen built in C++ sets a node's whole look with `NodeTree::SetLook`, which
takes a `NodeLook`: the node's own style, which properties of it the node
writes, the style it names and the tokens it uses.

`NodeTree::SetStyle` counts every property as the node's own, so a node styled
that way looks exactly as its `Style` says, whatever the theme.
