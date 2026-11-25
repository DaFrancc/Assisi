# Controls

Controls take all the attributes in [Layout and appearance](ui-layout.md). This
page lists the extra attributes each control is created with.

A toggle's `on` and a slider's `value` can hold a binding instead of a starting
value, and those three controls can push an event when the player moves them
with `on_change`. See [Controls](ui-bindings.md#controls) on the bindings page.

## Parts

A toggle, a slider and a scroll are drawn as **parts**. A part is an element
inside the control, such as a slider's thumb. You can style it like any other
element, and even put things in it. The control only decides where each part
goes and, for some parts, how big it is.

| Control | Parts |
|---|---|
| `toggle` | `track`, `knob` |
| `slider` | `track`, `fill`, `thumb` |
| `stepped_slider` | `track`, `fill`, `thumb` |
| `scroll` | `track`, `thumb` |

- **Toggle.** The `track` fills the toggle. The `knob` sits at the left end
  while the toggle is off and at the right end while it is on.
- **Slider.** The `track` runs down the middle. The `thumb` sits where the value
  is, and the `fill` covers the track from its start to the thumb's middle. The
  slider sets the fill's width.
- **Scroll.** Each scroll bar is a `track` with a `thumb` on it. The scroll sets
  both of their sizes. Both bars share one look: whatever you write for `thumb`
  styles the thumb of each.

### Styling a part

Write the part's name, then a `:`, in front of any attribute about how it looks:

```xml
<slider name="volume" step="5" thumb:background="#ffd23c" thumb:width="24"
        track:height="4" fill:background="#e63319" />
```

A part is in whatever [state](ui-layout.md#states) its control is in. While the
pointer is anywhere over the slider, its thumb is hovered too:

```xml
<slider step="5" thumb:background="#cccccc" thumb:hover:background="#ffffff" />
```

The part's name comes first, then the state. A toggle that is switched on is in
the `on` state, which is how you colour a toggle that's on:

```xml
<toggle name="vsync" track:background="#3a3f4b" track:on:background="#2fbf71" />
```

A part's own size comes from its `width` and `height`, like any element. The
control then moves it into place. So a wider thumb still stops at the end of
the slider, and a taller knob stays centred on the toggle.

To hide a part, write `visible="false"` on it. The control still works: a
slider with no track can still be dragged.

### Putting something in a part

Write a part's name as an element inside its control, and put elements in it.
Its attributes are the part's own, written without the prefix:

```xml
<slider name="volume" min="0" max="100" step="5" value="{=OptionsView.volume}">
  <thumb width="44" corner_radius="8">
    <text text_size="16">{OptionsView.volume:.0f}</text>
  </thumb>
</slider>
```

What is in a part moves with it, so the number above rides on the thumb.
Pressing it presses the slider, so a part can't hold a control such as a
button.

A scroll's elements are what it scrolls, so a scroll's parts can only be styled
with the prefix, as in `thumb:background`.

### Every slider at once

A theme can hold a style named after a kind of control, such as `slider`.
Every slider then wears it, with no `style` attribute needed. Your own `style`
and the element's own attributes go on top. See
[Styles for each kind of control](ui-themes.md#styles-for-each-kind-of-control).

## Toggle

```xml
<toggle name="fullscreen" on="true" />
```

| Attribute | Meaning |
|---|---|
| `on` | Whether it starts on, or a binding it follows. |
| `on_change` | The event it pushes when the player flips it. |

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
| `value` | The starting value, or a binding it follows. | The starting position, counted from 0, or a binding it follows. |
| `on_change` | The event it pushes when the player moves it. | The same. |

`slider` requires `step` because no default suits every range: a step of 0.1 is
reasonable on a 0–1 slider but would take a thousand presses on a 0–100 one.

A `stepped_slider` always moves one position per press, so it has no `step`.

To give a slider buttons that step it, put buttons beside it with the
[`step` action](ui-actions.md). Each press moves the slider as one arrow-key
press would:

```xml
<row gap="6">
  <button on_click="step(volume, -1)">-</button>
  <slider name="volume" min="0" max="100" step="5" />
  <button on_click="step(volume, 1)">+</button>
</row>
```

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

While a field has focus, its caret blinks. It stays visible while the player
types or moves it, and starts blinking again once they stop.

These colour the parts of a field that aren't text:

| Attribute | Values | Meaning |
|---|---|---|
| `caret_color` | colour | The caret. |
| `selection_color` | colour | The box behind selected text. Also used by `selectable` text, written on the text itself: a theme's `text_field` style dresses fields only. |
| `placeholder_color` | colour | The placeholder. Left out, it is the text colour, faded. |

Like any attribute about how something looks, they can go in a
[theme style](ui-themes.md) and take a state.

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
