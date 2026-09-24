# Elements and names

## Elements

Each element becomes one node. There are nine:

| Element | What it is | Holds text | Holds elements |
|---|---|---|---|
| `column` | A container that stacks its children top to bottom | No | Yes |
| `row` | A container that places its children left to right | No | Yes |
| `text` | Text | Yes | No |
| `button` | A button; its text is its label | Yes | No |
| `toggle` | An on/off switch | No | No |
| `slider` | A slider that can rest anywhere between its ends | No | No |
| `stepped_slider` | A slider that rests only on fixed positions | No | No |
| `scroll` | A container that scrolls its children instead of shrinking them | No | Yes |
| `text_field` | A box the player types into; its text is what it starts with | Yes | No |

Text goes between the tags: `<text>Paused</text>`. Text inside an element that
doesn't hold text, such as a `<row>`, fails the cook. So does an element inside
one that doesn't hold elements.

Any element can also have a `name` (see [Names](#names)) and `focus="true"` to
take keyboard focus when the screen opens. Only one element per screen may have
`focus="true"`.

The attributes every element takes are in [Layout and appearance](ui-layout.md).
The extra ones each control takes are in [Controls](ui-controls.md).

## Names

A `name` identifies a node so that code and other elements can refer to it:

- `Screen::Find` looks a node up by name from C++.
- A button's `step(...)` action names the slider it moves.
- A template instance's name is added to the names inside it.

The rules:

- **Names are optional.** Name the nodes you need to refer to and leave the rest
  unnamed.
- **A name must be unique on its screen.** Two nodes with the same name fail the
  cook, and the error gives both lines.
- **A name can't contain `.`.** The dot is used for names inside template
  instances (see [Templates](ui-templates.md#names-inside-a-template)).
- **A button's label is not its name.** Two buttons can both read "Back"; you
  find each by its `name`.
