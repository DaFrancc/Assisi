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
| `border_width`, `corner_radius`, `shadow_offset`, `shadow_blur`, `shadow_spread` | The element's own shorter side | The element's text size |
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

## Images

| Attribute | Values | Meaning |
|---|---|---|
| `background_image` | a sprite, `sheet:name` | A picture drawn across the whole element, over its `background`. |
| `tint` | colour | Multiplies the colours of every image the element draws. White, the default, leaves them as they are. |
| `fit` | `stretch`, `contain`, `cover` | How the element's picture fills the space inside its padding. |

See [Images](ui-images.md) for sprites, pictures and frames.

## Fading

`opacity` fades an element and everything inside it. It is a number from 0,
which shows nothing, to 1, the default, which shows everything:

```xml
<column opacity="0.5" padding="16" background="#1a1c24">
  <text>Half faded, words and all</text>
</column>
```

Opacity multiplies down the tree: a text at `opacity="0.5"` inside a panel at
`0.5` shows at a quarter. Each part fades on its own, so where two children
overlap, the one underneath shows through the one on top. To fade only an
element's own background and keep what is inside it solid, give the
background colour an alpha instead, such as `#1a1c2480`.

An element at `opacity="0"` draws nothing, but it still takes its space and
can still be clicked. To remove it, use `visible="false"`.

## Shadows and glows

An element can cast one shadow, drawn under it and following its corners,
square, rounded or cut:

```xml
<button corner_radius="12" corner_style="rounded" background="#2a2e3a"
        shadow_color="rgbf(0, 0, 0, 0.5)" shadow_offset="0 6" shadow_blur="16">Play</button>
```

| Attribute | Values | Meaning |
|---|---|---|
| `shadow_color` | colour | The shadow's colour. A fully clear colour (alpha 0), the default, draws no shadow. |
| `shadow_offset` | two lengths, `x y` | How far the shadow is moved from the element: right, then down. |
| `shadow_blur` | a length, not negative | How soft the shadow's edge is: it fades over about this far on each side of where a hard edge would be. `0` gives a hard edge. |
| `shadow_spread` | a length | How much bigger than the element the shadow is on every side. Negative makes it smaller. |

A **glow** is a shadow with no offset, in a bright colour:

```xml
<button shadow_color="#ffd84d" shadow_blur="12" hover:shadow_blur="20">Ready</button>
```

The whole shadow is drawn under the element, so it shows through a background
or frame that is partly see-through. Give an element with a shadow a solid
background.

`%` in these lengths is of the element's own shorter side, as for
`border_width`. Like every attribute on this page, they take states, so a
button can lift further while hovered, and they can come from a theme's
tokens.

The F9 tuning panel in the sandbox, `assets/ui/Tuning.amdn`, casts a shadow.

## Text

| Attribute | Values | Meaning |
|---|---|---|
| `text_size` | a length | Font size. |
| `text_color` | colour | Text colour. |
| `text_align` | `left`, `center`, `right` | Horizontal alignment of the text. |

An element that sets no `text_size` or `text_color` takes its parent's, so
setting either on a container sets it for everything inside. A screen's own
text size is 24 and its text colour white. A state passes down too: a row with
`hover:text_color` turns the text of every label inside it while the pointer
is over the row.

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
| `visible` | `true`, `false`, or a binding to a `bool` | Whether the element is shown. A hidden element takes no space. See [Showing values from the world](ui-bindings.md#showing-and-hiding). |
| `enabled` | `true`, `false` | Whether a control responds to the player. |
| `blocks_pointer` | `true`, `false` | Whether clicks on this element stop here instead of reaching the game. Use on a panel's background. |
| `takes_keyboard` | `true`, `false` | Whether, while focused, it receives the keyboard even when the game would otherwise have it. |
| `selectable` | `true`, `false` | Whether the player can select and copy its text. |
| `style` | a name | The [theme](ui-themes.md) style the element takes its look from. |

## States

An element can look different while the player is pointing at it, holding it
down or has moved to it with the keys, and while it is disabled. Write the
attribute again with the state in front of it and a `:` between them:

```xml
<button background="#222222" hover:background="#e63319"
        pressed:background="#a02010" disabled:text_color="#666666">Quit</button>
```

| State | Holds while |
|---|---|
| `focused` | The keys act on the element: the player moved to it with the arrow keys or Tab. |
| `hover` | The pointer is over the element, or over anything inside it. |
| `pressed` | The player is holding the element down, or holding a control inside it. Only a control can be held: a press on a `<text>` presses nothing. |
| `disabled` | The element has `enabled="false"`. |

Any attribute on this page can be given a state, including sizes and spacing.
The attributes about what an element is or does can't: `name`, `on_click`,
`focus`, `visible`, `enabled` and `style`.

Because `hover` holds for everything the pointer is inside, a whole row can
light up while the pointer is anywhere over it:

```xml
<row padding="8" gap="12" background="#1a1c24" hover:background="#2a2e3a">
  <text>Iron sword</text>
  <button>Equip</button>
</row>
```

When more than one state holds and they change the same attribute, the one
lower in the table wins: `disabled` over `pressed`, `pressed` over `hover`, and
`hover` over `focused`. A disabled button looks disabled even with the pointer
on it, and a button being held shows its pressed look rather than its hover
look.

`focused` shows only while the player is using the keys, as the focus ring
does. Clicking a button gives it focus without showing it. A text field is the
exception: it shows `focused` however it got focus, because the player is
typing into it.

A state that changes a size or spacing moves the elements around it, as the
same attribute would without a state. A button that grows on hover pushes its
neighbours aside, and if it grows out from under the pointer it stops being
hovered.
