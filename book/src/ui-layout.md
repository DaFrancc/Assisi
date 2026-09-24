# Layout and appearance

These attributes work on every element, including `<screen>` and the controls.

## Lengths

Every size, padding, gap, border, corner, offset and text size is a length. A
length is a number, optionally followed by a unit with no space between them:

| Written | Unit | Measured against |
|---|---|---|
| `20` | UI pixels | See below. |
| `50%` | Percent, 0 to 100 | The parent's content area along the same axis. |
| `5vw` | Percent of the screen's width | The screen. |
| `5vh` | Percent of the screen's height | The screen. |
| `1.5em` | Multiples of the text size | This element's text size. |

A few attributes measure `%` and `em` against something else:

| Attribute | `%` is of | `em` is of |
|---|---|---|
| `text_size` | The parent's text size | The parent's text size |
| `border_width`, `corner_radius` | The element's own shorter side | The element's text size |
| `float_offset` | The element it floats against | The element's text size |

There is no `px` suffix: a bare number already is one. `5px`, `5 %` (with a
space) and unknown units fail the cook.

A UI pixel is 1/1080 of the screen's shorter side, multiplied by the player's
UI scale setting:

| Screen | One UI pixel is |
|---|---|
| 1920×1080 | 1 screen pixel |
| 3840×2160 | 2 screen pixels |
| 3440×1440 (ultrawide) | 1.33 screen pixels |
| 1080×1920 (portrait) | 1 screen pixel |

So a design made for 1920×1080 looks the same size on any screen that is 1080
pixels on its shorter side, whatever its shape, and wider or taller screens
get extra space rather than a smaller UI. A game designed for only one
orientation can measure against the width or the height instead, with the
`uiScaleMatch` setting in [Game settings](game-settings.md).

## Size

`width` and `height` each take one of three forms:

| Form | Meaning |
|---|---|
| `fit` (default) | Just large enough for the content. |
| `grow` | Large enough for the content, plus a share of the space the parent has left over. |
| a length | Exactly that length, such as `420` or `50%`. |

Any form can be followed by `min` and `max` lengths to limit it:

```xml
<column width="420" height="fit">
<column width="grow min 10vh">
<column width="50% min 100 max 80vw">
```

## Arranging children

| Attribute | Values | Meaning |
|---|---|---|
| `direction` | `row`, `column` | How children are arranged. `<row>` and `<column>` set this already; `<screen>` is a column. |
| `padding` | a length, or `left top right bottom` | Space between the element's edge and its children. |
| `gap` | a length | Space between children. |
| `align` | two words: horizontal, then vertical | Where children sit. Each word is `start`, `center` or `end`. |

## Colours, borders and corners

| Attribute | Values | Meaning |
|---|---|---|
| `background` | colour | Fill colour. |
| `border_width` | a length | Border thickness. |
| `border_color` | colour | Border colour. |
| `corner_radius` | a length | Size of the corners. `50%` on a square makes a circle. |
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

## Text

| Attribute | Values | Meaning |
|---|---|---|
| `text_size` | a length | Font size. |
| `text_color` | colour | Text colour. |
| `text_align` | `left`, `center`, `right` | Horizontal alignment of the text. |

## Floating

A floating element is placed on top of the others instead of taking space in its
parent — a badge on a corner, for example.

| Attribute | Values | Meaning |
|---|---|---|
| `float` | `true`, `false` | Whether the element floats. |
| `float_target` | `parent`, `root` | What it is placed against: its parent, or the whole screen. |
| `float_anchor` | two alignment words | The point on the target to attach to. |
| `float_attach` | two alignment words | The point on the element that goes on the anchor. |
| `float_offset` | two lengths, `x y` | A further shift after attaching. |
| `float_clip` | `true`, `false` | Whether the parent's edges clip it. |

## Scrolling

These set how a `<scroll>` element (see [Scroll](ui-controls.md#scroll)) looks
and moves:

| Attribute | Values | Meaning |
|---|---|---|
| `scroll_bar_visibility` | `never`, `when-needed`, `always` | When the scroll bar is shown. `when-needed` shows it only while some content is out of view. |
| `scroll_bar_drag` | `follows-pointer`, `smoothed` | Whether content moves with the dragged bar exactly, or glides after it. |
| `scroll_smoothing` | seconds | How long scrolling takes to reach its destination. `0` jumps immediately. |
| `scroll_bar_min_length` | a length | The shortest the scroll bar's handle can be. |

## Behaviour

| Attribute | Values | Meaning |
|---|---|---|
| `visible` | `true`, `false` | Whether the element is shown. A hidden element takes no space. |
| `enabled` | `true`, `false` | Whether a control responds to the player. |
| `blocks_pointer` | `true`, `false` | Whether clicks on this element stop here instead of reaching the game. Use on a panel's background. |
| `takes_keyboard` | `true`, `false` | Whether, while focused, it receives the keyboard even when the game would otherwise have it. |
| `selectable` | `true`, `false` | Whether the player can select and copy its text. |
| `style` | a name | Reserved for themes. Accepted and stored, but has no effect yet. |
