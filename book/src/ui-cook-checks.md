# What the cook checks

Every one of these fails the cook with the file, line and column:

- An unknown element, attribute or value.
- Text or child elements inside an element that can't hold them.
- A length with an unknown unit, a `px` suffix, or a space before its unit.
- A colour written as bare numbers, or with a channel outside its call's range.
- A `slider` without a `step`, or with `step="0"`.
- A pattern name that doesn't exist, or an expression that isn't valid.
- Two nodes with the same name, or a name containing `.`.
- More than one element with `focus="true"`.
- An `on_click` on anything other than a button.
- An event that no header under `apps/game/src/` declares.
- An action that doesn't exist, has the wrong number of arguments, or is written
  without parentheses.
- A `step` target that doesn't exist on the screen or isn't a slider, or a
  `step` of 0 moves.
- A template that is declared in the wrong place, is badly formed, contains
  itself, or nests more than eight deep.
- An unnamed instance that would repeat names made by another.
- A template parameter named like an attribute or declared twice, an instance
  that leaves out a parameter with no default, an `@name` the template doesn't
  declare, or an `@` inside a template that isn't `@@` or a parameter.
- An import whose path names no file or isn't a `.amdt` library, a name in
  `names` that the library doesn't declare, a template name used that nothing
  imported, an imported name the file already has, two imports sharing an `as`
  prefix, or a library that imports itself.
- A screen file whose root isn't `<screen>`, or a library whose root isn't
  `<templates>` or that holds anything but templates and imports.

An error inside a template library names the library's file.

## What's next

Themes, for keeping colours and sizes out of the layout, and data bindings, for
showing values from the world, are planned next.
