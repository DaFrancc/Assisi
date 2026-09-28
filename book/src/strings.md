# Strings

The engine has one string type for each job text does. This page lists them,
shows how to choose one, and shows how each is saved.

## The types

| Type | Holds | Stored as |
|---|---|---|
| `Core::InternedString` | Names the engine compares and a player never reads: node, style, event, system and asset names | An index into one table of every name, shared by the whole program |
| `Core::DisplayedString` | Words a player reads: labels, descriptions, tooltips | A string-table key such as `#pause:title`, or literal text |
| `Core::PooledString` | One of many strings that belong to one owner, such as a cooked screen | An offset and a length into the owner's `Core::StringPool` |
| `Core::ShortString`, `Core::EntityName` | Short text inside a component that must stay a fixed size | The text itself, in a fixed buffer of 32 or 64 bytes |
| `std::string`, `std::string_view` | Text that changes while the game runs, and text in tools; `std::string_view` for parameters | Ordinary C++ strings |

## Choosing one

Ask what the text is for:

- **Compared, never shown?** Use `InternedString`.
- **Read by a player?** Use `DisplayedString`.
- **One of many owned by one asset?** Use `PooledString`.
- **Short, and has to live inside a component?** Use `ShortString` or `EntityName`.
- **Changes while the game runs?** Use `std::string`.

A reflected component can't hold a `std::string`; the build names these types
instead.

## InternedString

```cpp
const Core::InternedString style{"heading"};
if (node.style == style) { /* ... */ }
```

The first time a text is seen it is stored in a table, and every
`InternedString` made from the same text holds the same number. Comparing two
of them compares two integers, not two strings. `View()` gives the text back.

- The table only grows. Names are never removed.
- `View()` can be called from any thread.
- The number is not the same from one run to the next, so it is never saved or
  sent. Files and packets carry the text.
- An `InternedString` can be an element of a list, but not the key of a map:
  it has no fixed order.

## DisplayedString

```cpp
const Core::DisplayedString title = Core::DisplayedString::FromKey("pause", "title");
const Core::DisplayedString note  = Core::DisplayedString::FromLiteral("Debug build");
std::string words = title.Resolve();   // "Paused", from the table ui/pause.csv
```

A `DisplayedString` is written the same way as text in a screen (see
[String tables](ui-string-tables.md)):

- `#pause:title` is the key `title` in the table `pause`.
- `##1 fan` is the literal text `#1 fan`.
- Anything else is literal text.

`Resolve()` gives the words to show. A key the tables don't have shows as the
key itself, `#pause:title`, so it is easy to spot. In a file, a
`DisplayedString` is saved exactly as written.

Text sent over the network is limited to 4096 bytes. Longer text belongs in a
string table, with the key in the `DisplayedString`.

## PooledString and StringPool

```cpp
ACOMP()
struct Credits
{
    AFIELD() Core::StringPool pool;
    AFIELD() std::vector<Core::PooledString> names;
};

Credits credits;
credits.names.push_back(credits.pool.Add("Ada"));
credits.names.push_back(credits.pool.Add("Grace"));
std::string_view first = credits.pool.View(credits.names[0]);   // "Ada"
```

A `StringPool` keeps its strings end to end in one buffer, and each
`PooledString` is an offset and a length into it. Reading every name in
`credits.names` reads one block of memory, not one allocation per name.

- **A struct with any `PooledString` holds exactly one `StringPool`.** The
  handle doesn't say which pool it belongs to, so the build refuses a struct
  with none or with two.
- Strings are only added. Changing one adds its new text and points the handle
  at it, leaving the old bytes in the pool.
- A handle that points outside its pool reads as an empty string.
- In a file, the pool is saved as one string and each handle as its offset and
  length: `{ "offset": 0, "length": 3 }`. Pools are meant to be written by the
  cook, not by hand.
