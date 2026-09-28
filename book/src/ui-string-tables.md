# String tables

A screen can show its words as written, or look them up in a **string table**.
A table keeps a screen's words in one place, where a translator can find them.
A game that will be translated should use tables from the start.

## Writing text as it is

Text written between the tags is shown as it is:

```xml
<text>Paused</text>
```

Writing literal text rather than referencing a string table is only recommended
for debug/testing purposes or small projects.

## Looking text up in a table

Text that starts with `#` is a **key**. A key names a table and an entry in it,
separated by a colon (`:`):

```xml
<text>#pause:title</text>
<button on_click="hide()">#pause:resume</button>
<text_field placeholder="#pause:hint" />
```

`#pause:title` means the entry `title` in the table `pause`. Keys work in the
text of a `text`, a `button` and a `text_field`, and in a field's `placeholder`.

To start text with a real `#`, write it twice: `<text>##1 fan</text>` shows
`#1 fan`. A `#` anywhere other than the start is shown as it is.

A key can be passed to a template like any other value:

```xml
<menu_button label="#pause:resume" />
```

## Table files

A table is a `.csv` file, which any spreadsheet program can edit. The first row
is a header: `key`, then a column for the text. Every row after it is one entry:

```
key,en
title,Paused
resume,Resume
quit,Quit
```

- **The table's name is its file name**, without the folder or the extension:
  `ui/pause.csv` is the table `pause`.
- **Each column after `key` is one language.** The game can't switch languages
  yet, so it uses the first of them. The others are kept in the file but not
  read, so translations can be added now and used once language selection
  exists:

  ```
  key,en,fr
  title,Paused,En pause
  ```
- **Text with commas or line breaks** goes in double quotes: `"Yes, quit"`. A
  quote inside quotes is written twice: `"Say ""hi"""`.
- **Save as CSV UTF-8** with commas between columns. Plain "CSV" in some
  spreadsheet programs saves another encoding, which breaks accented and
  non-Latin text.

Many small tables are easier to work with than one large file: each screen or
feature can have its own, and it also helps avoid edit conflicts as changes 
are spread out.

## Listing tables

A `.csv` file is a string table only when `assets/config/ui.json` lists it:

```json
{
  "version": 1,
  "type": "UiConfig",
  "stringTables": ["ui/pause.csv", "text/quests.csv"],
  "requireStringKeys": false,
  "allowEmptyStrings": false
}
```

| Setting | What it does |
|---|---|
| `stringTables` | Every string table the game has. Two tables with the same file name fail the cook. |
| `requireStringKeys` | When `true`, any text that is not a key fails the cook, so no text is missed when the game is translated. |
| `allowEmptyStrings` | When `false` (the default), an entry with no text fails the cook, since it is usually text nobody wrote yet. |

## Requiring keys

With `requireStringKeys` on, text written as it is fails the cook in every place
a key could go. This includes text with no letters, such as `100%`, because
numbers and symbols are also written differently in some languages.

Only text written in screen files is checked. Text that arrives while the game
runs is not: chat messages and other text code sets with `SetText`, and
whatever a player types into a text field.

A screen players never see, such as a debug overlay, can be exempted:

```xml
<screen debug_only="true">
  <text>frame time</text>
</screen>
```

A template library belongs to no screen, so its text is never exempt.

## Using keys from C++

The game loads every table `config/ui.json` lists when it starts.

To show a table's text on a node from code, give the node a key:

```cpp
screen.SetTextKey(node, "pause:title");
```

The cook checks keys in screen files, but not keys in code. A key in code that
no table has shows on screen as the key itself, such as `#pause:title`, so the
mistake is visible rather than blank.

## Replacing the tables

`Ui::SetStringTables` replaces the tables the UI reads from. Every node showing
a key, on every screen, updates at once. There is no language setting yet; when
there is, it will switch languages this way.

Two kinds of text are not updated:

- **Text set with `SetText`.** It belongs to the code that set it, even if the
  node showed a key before.
- **A text field's contents.** A key there gives the field its starting text,
  which the player can then change, so it is looked up only once.
