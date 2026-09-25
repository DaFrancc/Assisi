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

## Parameters

A template can take **parameters**: values each instance passes in. Use them
when the template needs something only the instance knows, such as which slider
a button moves.

```xml
<template name="stepper" params="target moves=1">
  <button padding="12 4 12 4" text_size="28" on_click="step(@target, @moves)" />
</template>

<slider name="volume" min="0" max="100" step="5" />
<stepper target="volume" moves="-1">-</stepper>
<stepper target="volume">+</stepper>
```

### Declaring

`params` lists the parameter names, separated by spaces. `name=value` gives a
parameter a default, which makes it optional. A default is one word, with no
spaces in it.

A parameter name is made of letters, digits and `_`, and doesn't start with a
digit. It can't be the name of an attribute the markup has, such as `name`,
`width` or `on_click`, so an attribute on an instance is always either a
parameter or an override of the template's root.

### Passing

An instance passes a parameter as an attribute of the same name. A parameter
without a default must be passed by every instance.

### Using

Inside the template, `@name` stands for the parameter's value. It can be used in
any attribute value or text, on its own or as part of a longer value:

```xml
<template name="caption" params="size label">
  <text text_size="@size">The @label here</text>
</template>
```

Inside a template, `@` is a special character, like `\` in a C string: it
always starts a parameter name, so it can't stand for itself. To write an
actual `@` character, such as in an email address, write it twice:

```xml
<template name="contact">
  <text>support@@example.com</text>   <!-- shows support@example.com -->
</template>
```

A single `@` that isn't followed by a parameter name fails the cook. Outside
templates, `@` has no special meaning and is written as it is.

The cook replaces every `@name` with its value, so the compiled screen is the
same as one written out by hand.

### Which node a passed name means

A node name in a parameter's value is looked up where the value was written:

- **A value passed by the instance** was written where the instance is, so it
  means a node there. In the example above, `target="volume"` means the
  screen's `volume`, not a node called `volume` inside the instance.
- **A default** was written in the template, so it means a node inside the same
  instance, like any other name the template writes.

## Template rules

- A template is declared directly inside `<screen>`, nowhere else. To share one
  between screens, declare it in a [template library](ui-template-libraries.md).
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
- An attribute that uses a parameter without a default can only be checked once
  an instance passes a value, so it is checked for each instance.
