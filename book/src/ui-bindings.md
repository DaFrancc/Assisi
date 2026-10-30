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
  <context on_lost="hide()" />

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
    hud->SetContext(Assisi::App::ScreenEntity(gained.entity));
    hud->Show();
    (void)ctx.world.scene.Add(gained.entity, PlayerHud{.screen = Assisi::App::IdOf(ctx.world, *hud)});
}
```

`PlayerHud` is the game's own component, holding the HUD's `ScreenId`, so the
character keeps a reference to its HUD (see
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
| `bool` | `true` or `false` | |
| An `AENUM` enum | The enumerator's name | `>8` to right-align in 8 characters |
| `ShortString`, `InternedString` | The text | |
| `glm::vec2`, `vec3`, `vec4` | `x, y, z`, each with the format | `.1f` |

The format is what comes after the `:` in C++'s `std::format`, such as `.2f` or
`>6`. A number that shows as zero shows without a minus sign: `-0.04` with
`.1f` reads `0.0`.

Other field types, such as entities, matrices and lists, can't be shown, and
fail the cook.

## Showing and hiding

`visible` can be bound to a `bool` field. The element shows while it is `true`:

```xml
<text visible="{CharacterState.sprinting}">Sprinting</text>
```

`visible` takes one binding and nothing else, with no format.

## Where bindings go

- Inside a `<text>`, and in a `<button>`'s label.
- In `visible`.

A text field's text is the player's, so it can't hold a binding, and neither can
a placeholder.

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
