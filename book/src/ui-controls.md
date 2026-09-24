# Controls

Controls take all the attributes in [Layout and appearance](ui-layout.md). This
page lists the extra attributes each control is created with.

## Toggle

```xml
<toggle name="fullscreen" on="true" />
```

| Attribute | Meaning |
|---|---|
| `on` | Whether it starts on. |

## Slider and stepped slider

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

## Scroll

```xml
<scroll name="list" axes="y" height="150" scroll_bar_visibility="when-needed">
  <button>One</button>
  <button>Two</button>
</scroll>
```

| Attribute | Values | Meaning |
|---|---|---|
| `axes` | `x`, `y`, `xy`, `none` | Which directions it scrolls in. |

Other elements write this setting as `scroll_bars`. On a `<scroll>`, only
`axes` is accepted, so the setting can't be written twice.

How the bar looks and moves is set by the
[scrolling attributes](ui-layout.md#scrolling).

## Text field

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

## Patterns

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
