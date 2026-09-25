# Template libraries

A template declared inside a `<screen>` belongs to that screen. To share
templates between screens, such as a menu button every menu uses, put them in a
**template library** and import it.

## Writing a library

A library is a file ending in `.amdt`. Its root is `<templates>`, and it holds
one or more `<template>` declarations:

```xml
<!-- assets/ui/common/Controls.amdt -->
<templates>
  <template name="menu_button">
    <button padding="28 10 28 10" text_size="28" />
  </template>
  <template name="labelled_slider">
    <row>
      <menu_button name="down" on_click="step(slider, -1)">-</menu_button>
      <slider name="slider" step="5" />
    </row>
  </template>
</templates>
```

Templates in a library are written exactly like templates in a screen, and can
take [parameters](ui-templates.md#parameters) in the same way. A template can
use the other templates in its own library.

| File | Root | Holds | Cooks to |
|---|---|---|---|
| `.amdn` | `<screen>` | A screen, and templates only it can use | A screen |
| `.amdt` | `<templates>` | Templates for other files to import | Nothing of its own |

## Importing a library

A screen imports a library with `<import>`, written directly inside
`<screen>`. The path is relative to `assets/`, like every asset path:

```xml
<screen>
  <import path="ui/common/Controls.amdt" />
  <menu_button name="resume" on_click="hide()">Resume</menu_button>
</screen>
```

A library can import another library the same way, directly inside
`<templates>`. What a library imports is for its own templates to use; it isn't
passed on to the files that import the library.

### Choosing templates with `names`

Without `names`, every template the library declares is imported. With it, only
the templates listed, separated by spaces:

```xml
<import path="ui/common/Controls.amdt" names="labelled_slider" />
```

A listed template still works if it is built on one that isn't listed. Here
`labelled_slider` still uses `menu_button`, but the screen itself can't write
`<menu_button>`.

### Prefixing names with `as`

`as` puts the imported names under a prefix:

```xml
<import path="ui/common/Dialogs.amdt" as="dialogs" />
<dialogs.confirm name="really_quit" />
```

With `as="dialogs"`, the template is written `<dialogs.confirm>`, and plain
`<confirm>` doesn't exist. Use a prefix when two libraries declare templates with
the same name. `names` and `as` can be used together: `names` chooses what comes
in, and `as` chooses what it is called.

The `.` in `<dialogs.confirm>` is part of the element's name. It is not the same
as the `.` in a node name such as `music.slider`, which comes from the instance
the node is inside (see [Templates](ui-templates.md#names-inside-a-template)).
One appears in element names and the other in `name` attributes, so they never
meet.

## Import rules

- `<import>` sits directly inside `<screen>` or `<templates>`, nowhere else. It
  takes `path`, and optionally `names` and `as`, and holds nothing.
- `path` must name a `.amdt` file that exists. A screen's own templates can't be
  imported; move templates you want to share into a library.
- Each template name can mean only one template in a file. An imported name
  that matches one of the file's own templates, or one another import brought
  in, fails at the second import. Two imports can't use the same `as` prefix.
- A library can't import itself, directly or through other libraries.
- Imported templates count toward the limit of eight templates nested inside
  each other.

## Cooking

A library cooks to nothing of its own. Its templates are copied into each screen
that uses them, and the compiled screen is identical to one that declared those
templates itself.

Every template in a library is checked when the library is cooked, even if no
screen uses it. An error inside a library names the library's file, line and
column.

A screen depends on every library it imports, including libraries imported by
those libraries. Changing a library cooks every screen that uses it again.
