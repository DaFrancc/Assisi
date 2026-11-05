# Showing values from the world

A screen can show a value from an entity, such as the player's speed or a boss's
health, and keep it up to date with no code. Write the component and field in
braces in an element's text:

```xml
<text>{CharacterState.groundSpeed:.1f} m/s</text>
```

This is a **binding**. Which entity it reads is the screen's context, which code
sets (see [Contexts](ui-run-time.md#contexts)).

## A HUD

The sandbox's HUD, `assets/ui/Hud.amdn`, shows the player's speed and velocity
in the bottom-right corner, and "Sprinting" while the player sprints:

```xml
<screen sort="hud" align="end end" padding="24">
  <column style="panel" padding="16" gap="4" align="end center">
    <text text_size="28">{CharacterState.groundSpeed:.1f} m/s</text>
    <text text_size="18">{CharacterState.velocity:.1f}</text>
    <text text_size="18" visible="{CharacterState.sprinting}">Sprinting</text>
  </column>
</screen>
```

The only code is a system that gives each of this machine's players a HUD of its
own when it becomes one, in `apps/game/src/Hud.cpp`:

```cpp
for (const Assisi::App::LocalPlayerGained &gained : ctx.events.Read<Assisi::App::LocalPlayerGained>())
{
    Assisi::Mondrian::Screen *const hud = Assisi::App::LoadScreen(ctx.world, *ctx.ui, "ui/Hud.amdn");
    if (hud == nullptr)
    {
        continue;
    }
    const Assisi::App::ScreenId id = Assisi::App::IdOf(ctx.world, *hud);
    hud->SetContext(Assisi::App::ScreenEntity(gained.entity));
    hud->OnContext({}, Assisi::Mondrian::ContextMoment::Lost, HudOrphaned{.screen = id});
    hud->Show();
    (void)ctx.world.scene.Add(gained.entity, PlayerHud{.screen = id});
}
```

`PlayerHud` is the game's own component, holding the HUD's `ScreenId`, so the
character keeps a reference to its HUD. `HudOrphaned` is the game's own event,
which the HUD pushes if the character is destroyed, so a second system can
remove the screen (see
[Screens an entity owns](ui-run-time.md#screens-an-entity-owns)).

## Writing a binding

| Written | Reads |
|---|---|
| `{CharacterState.groundSpeed}` | That field of the context entity's component. |
| `{boss:Health.current}` | The same from the context called `boss`, which the screen declares with `<context name="boss" />`. |
| `{Health.current:.0f}` | The same, shown with a format. |
| `{{` and `}}` | One `{` and one `}`, shown as they are. |

Bindings and words can be mixed: `HP {Health.current:.0f} of {Health.max}`.

The component and the field are the names reflection gives them: the struct's
name without its namespace, and the member's name.

## What can be shown

| Field type | Shown as | Example format |
|---|---|---|
| `float`, `double` | A number | `.1f` for one decimal place |
| Whole numbers | A number | `+` to always show the sign, `03` to pad with zeros |
| `bool` | `true` or `false`, or words from a [string table](#words-from-a-string-table) | |
| An `AENUM` enum | The enumerator's name, or words from a [string table](#words-from-a-string-table) | `>8` to right-align in 8 characters |
| `ShortString`, `InternedString` | The text | |
| `glm::vec2`, `vec3`, `vec4` | `x, y, z`, each with the format | `.1f` |

The format is what comes after the `:` in C++'s `std::format`, such as `.2f` or
`>6`. A number that shows as zero shows without a minus sign: `-0.04` with
`.1f` reads `0.0`.

Other field types, such as entities, matrices and lists, can't be shown, and
fail the cook.

## Words from a string table

An enumerator's name comes from the code, so a player sees `Hard` however the
game is translated, and a `bool` reads `true` or `false`. A key after the `:`
shows them through a [string table](ui-string-tables.md) instead:

```xml
<text>Difficulty: {TuningSettings.difficulty:#tuning:difficulty}</text>
<text>{TuningSettings.showHud:#tuning:switch}</text>
```

`#tuning:difficulty` means the table `tuning`, and entries starting with
`difficulty`. The value picks the entry: the key, a `.`, then the enumerator's
name, or `true` or `false`. The sandbox's `ui/tuning.csv` holds them:

```
key,en
difficulty.Easy,Gentle
difficulty.Normal,Steady
difficulty.Hard,Brutal
switch.true,On
switch.false,Off
```

So when `difficulty` holds `Hard`, the binding shows the entry `difficulty.Hard`,
which is `Brutal`.

- Entries go by name, so reordering an enum keeps its words, and renaming an
  enumerator fails the cook until its entry is renamed too.
- The table must have an entry for every enumerator, or for both `true` and
  `false`, or the cook fails. An enumerator added to the enum needs its words
  before the game can cook.
- Two screens can word one enum differently: each binding names its own key.
- An enum value with no enumerator shows as its number.
- On a number, `#` is `std::format`'s: `{Gauge.count:#x}` shows `0xff`. A key on
  any other type fails the cook.
- [Replacing the string tables](ui-string-tables.md#replacing-the-tables) reaches
  this text at the next sync.

This is how a `stepped_slider` over an enum gets a translated label: bind the
same field in a text beside it, with a key.

## Showing and hiding

`visible` can be bound to a `bool` field. The element shows while it is `true`:

```xml
<text visible="{CharacterState.sprinting}">Sprinting</text>
```

`visible` takes one binding and nothing else, with no format.

## Where bindings go

- Inside a `<text>`, and in a `<button>`'s label.
- In `visible`.
- In a toggle's `on`, and in the `value` of a slider or a stepped slider.

A text field's text is the player's, so it can't hold a binding, and neither can
a placeholder. `{=...}` goes only on a control.

## When values change

Bound values are read every frame, just before the screen is laid out, so what
is drawn is what the world holds that frame.

- Before a binding has read anything, it shows nothing: only the words around it.
  A bound `visible` starts hidden.
- When the context has no entity, or its entity has been destroyed, a binding
  keeps what it last showed. A boss's health bar stays at 0 when the boss dies,
  until something hides it, for example `<context name="boss" on_lost="hide()" />`.
- When the entity has no such component, the binding keeps what it showed and an
  error is logged once.

## Controls

A toggle, a slider or a stepped slider can follow a field too. Write the binding
where the control's value goes: `on` on a toggle, and `value` on a slider or a
stepped slider.

```xml
<slider value="{=Camera.fovDegrees}" min="40" max="120" step="5" />
```

`{=...}` is a **two-way** binding: the control shows the field, and moving the
control writes the field. `{...}` without the `=` is **one-way**: the control
shows the field and writes nothing.

| Control | Field types it holds |
|---|---|
| `toggle` | `bool` |
| `slider` | Numbers. A whole number is rounded. |
| `stepped_slider` | Whole numbers, which hold the position counted from 0, and `AENUM` enums. |

A control's binding takes no format, since it shows no text.

A `stepped_slider` bound to an enum has one position per enumerator, in the order
the enum declares them, so it writes no `steps`, `min` or `max`. To show which
one is chosen, bind the same field in a text beside it:

```xml
<text>Difficulty: {TuningSettings.difficulty:#tuning:difficulty}</text>
<stepped_slider value="{=TuningSettings.difficulty}" />
```

Bound to a whole number, a `stepped_slider` still says how many positions it has
with `steps`.

### When a control reads and writes

- A control reads its field every frame, just before the screen is laid out, the
  same as any other binding.
- It writes only when the player moves it. Showing a field never writes it, even
  when the field holds more than the control's range can show.
- While the player drags a control, it shows what the player is doing, not the
  field. If a system changes the field during the drag, the drag isn't
  interrupted. When the player lets go, the control shows the field again.
- With no entity, no component or no source to write to, the move is dropped.

### Telling the game about a change

`on_change` names an event to push each time the player moves the control:

```xml
<toggle on="{=OptionsView.vsync}" on_change="Game::OptionsChanged" />
```

A field changing on its own does nothing else, so this is how a game saves
settings or rebuilds what depends on them. The event is pushed after the field
has been written, so a system reading it sees the new value.

If the event has a field called `value`, it receives what the control holds: a
`bool` from a toggle, a `float` from a slider, and an `int32_t` position from a
stepped slider.

```cpp
AEVENT()
struct DifficultyChanged
{
    int32_t value = 0;
};
```

An event with no `value` field is pushed as it is. One whose `value` is any other
type fails the cook.

### Controls in a multiplayer game

A two-way binding writes a component exactly as `GetMut` does in a system, so it
behaves the same in a networked game:

- On a host, in single-player and in the editor, the write sticks, and a host
  replicates it like any other change.
- On a client, a write to a component the server owns is replaced by the next
  snapshot, and a write to a predicted component is lost at the next rollback.
  The slider jumps back.

For data the server has to approve, use a one-way binding and `on_change`, and
have a system ask the server:

```xml
<slider value="{Turret.aim}" min="-90" max="90" step="1"
        on_change="Game::AimRequested" />
```

The control shows what the world holds, and only the system changes it. If
nothing applies the change, the control returns to the field's value.

## Fields on no entity

Some values belong to no entity, such as the settings in an options menu. A
reflected struct can be registered with the UI as a **source**, and bound the same
way as a component:

```cpp
ASTRUCT()
struct TuningSettings
{
    AFIELD() Difficulty difficulty = Difficulty::Normal;
    AFIELD() bool showHud = true;
};
```

```cpp
ASSISI_REFLECTED_STRUCT(Game::TuningSettings); // at global scope, before it is used

ctx.ui->RegisterSource(settings);
```

```xml
<toggle on="{=TuningSettings.showHud}" />
<column visible="{TuningSettings.showHud}">...</column>
```

The name in a binding is the struct's name without its namespace. A source needs
no context, and giving it one fails the cook.

- Every screen sees a source, whichever world it belongs to, so two screens
  binding one struct share it. In the sandbox, F9 shows `ui/Tuning.amdn`, whose
  toggle and slider change what `ui/Hud.amdn` shows behind it.
- The UI keeps a pointer to what you register, so it must outlive the UI or be
  taken away first with `ui.UnregisterSource<TuningSettings>()`.
- There is one source per struct type. Registering another replaces the first.
- A screen can be made before its source is registered. If nothing has been
  registered by the time the screen is first drawn, an error is logged once.
- After a source is unregistered, its bindings keep what they last showed and
  moves are dropped, the same as a context with no entity.

## Bindings and string tables

With `requireStringKeys` on, the words around a binding fail the cook, because
they are text a player reads. A text holding only bindings and spaces passes.
Put the words in a text of their own, with a key:

```xml
<row gap="4">
  <text>{Health.current:.0f}</text>
  <text>#hud:hit_points</text>
</row>
```

An enum or a `bool` bound with no key fails the cook too, because its word comes
from the code. Give it [a key](#words-from-a-string-table). Numbers and strings
pass.
