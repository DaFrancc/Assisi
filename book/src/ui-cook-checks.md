# What the cook checks

Every one of these fails the cook with the file, line and column:

- An unknown element, attribute or value.
- A state that isn't `focused`, `hover`, `pressed` or `disabled`, two states on
  one attribute, a state on an attribute about what an element is or does such
  as `on_click`, or a `:` with no name on one side of it.
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
- A key with no table before it, a table `config/ui.json` doesn't list, or an
  entry the table doesn't have.
- Text written as it is, when `requireStringKeys` is on and the screen isn't
  `debug_only`.
- A `style` one theme `config/ui.json` lists doesn't have, a `$token` one
  listed theme doesn't have, or one whose value in some mode its attribute
  can't hold. A style or a token used while `ui.json` lists no theme. A state
  on `style`, or a token for an attribute about what an element is or does.

An error inside a template library names the library's file. An error about a
node's text points at the element that wrote it, or at the `placeholder`
attribute.

These fail the cook of a string table, with the line:

- A first row that isn't a header starting with `key`, or has no text column.
- A row with no text column, no key, or a key already in the table.
- An entry with no text, unless `allowEmptyStrings` is on.
- A quote that is never closed, or text after a closing quote.

Two listed tables with the same file name, or a listed table that isn't
there, fail the cook of every screen.

These fail the cook of a theme, with the line:

- A root that isn't `<theme>`, an attribute on it, or anything in it but
  `<palette>` and `<style>`.
- No palette, a palette with no `mode`, two palettes for one mode, or a palette
  holding anything.
- A token one palette gives and another doesn't, a token's name that isn't
  letters, digits and `_`, or a token's value that names another token.
- A style with no `name`, two styles with one name, or a style holding anything.
- A style's attribute that is unknown, belongs to one kind of control, or says
  what a node is or does, or a value the attribute can't hold.
- A style's `$token` the palettes don't give, or give a value in some mode its
  attribute can't hold.
- An `extends` naming a style the theme doesn't declare, or a chain of them
  that comes back round to where it started.

A listed theme that isn't there, or doesn't compile, fails the cook of every
screen.

## What's next

Data bindings, for showing values from the world, are planned next.
