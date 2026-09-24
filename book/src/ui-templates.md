# Templates

A template lets you define an element once and reuse it. Use templates when
several parts of a screen share a look or a structure.

## Declaring and using a template

A template is declared directly inside `<screen>`. It has a name and one **root
element**, which can hold any number of elements inside it (see the
`labelled_slider` example below). Here the root is a single button:

```xml
<template name="menu_button">
  <button padding="28 10 28 10" text_size="28"
          corner_radius="10" corner_style="rounded" />
</template>
```

The template's name can then be used as an element anywhere on that screen,
before or after the declaration. Each use is called an **instance**:

```xml
<menu_button name="resume" on_click="hide()" background="#e63319">Resume</menu_button>
<menu_button name="quit" on_click="Assisi::App::QuitRequested"
             border_width="2" border_color="#ffffff">Quit</menu_button>
```

An instance is a copy of the template's element, changed by the instance:

| On the instance | Effect |
|---|---|
| Attributes | Replace the template's attribute of the same name. Attributes the instance doesn't set come from the template. |
| Text | Replaces the template's text, if the instance has any. |
| Child elements | Are added after the template's children. |

The cook replaces every instance with ordinary elements. The compiled screen is
identical to one written out by hand, so nothing at run time knows templates were
used.

## Names inside a template

Nodes inside a template can have names, and actions inside it can refer to them:

```xml
<template name="labelled_slider">
  <row>
    <text name="label" />
    <button name="down" on_click="step(slider, -1)">-</button>
    <slider name="slider" min="0" max="100" step="5" />
  </row>
</template>

<labelled_slider name="music" />
<labelled_slider name="effects" />
```

Each instance adds its own name and a dot in front of the names inside it. This
example creates `music.label`, `music.down`, `music.slider`, `effects.label`,
and so on.

### Which node an action's target means

An action's target is looked up where the action is written:

- **Written inside the `<template>`**, a target means a node in the same
  instance. The template's `step(slider, -1)` becomes `step(music.slider, -1)`
  in the `music` instance and `step(effects.slider, -1)` in the `effects`
  instance, so each `-` button moves its own slider.
- **Written anywhere else in the screen**, a target means a node on the screen,
  and a node inside an instance needs its full name. This includes attributes
  on the instance element itself, because they are written in the screen, not
  in the template.

```xml
<screen>
  <labelled_slider name="music" />

  <!-- Written in the screen: the full name is needed. -->
  <button on_click="step(music.slider, 1)">+</button>   <!-- moves music.slider -->
  <button on_click="step(slider, 1)">+</button>         <!-- refused: the screen has no node called "slider" -->
</screen>
```

An instance without a name leaves the names inside unchanged. That works once;
a second unnamed instance would create the same names again, so the cook asks
you to name it. A template with no names inside can be used without a name any
number of times.

## Template rules

- A template is declared directly inside `<screen>`, nowhere else.
- Its name can't be an existing element name such as `button`, and two templates
  can't share a name.
- It has exactly one root element and no text of its own. The root can hold
  any number of elements. There is one root because an instance becomes one
  node, and the instance's attributes, text and children apply to that root.
  To group several elements, wrap them in a `<row>` or `<column>`.
- That element must be a built-in element. It can contain instances of other
  templates, but can't itself be one.
- A template can't contain itself, directly or through other templates.
- Templates can be nested up to eight deep.
- Every template is checked by the cook even if nothing uses it. An error inside
  one is reported at its line; if the error only appears in a particular
  instance, the message also gives the instance's line.
