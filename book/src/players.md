# Players

A **player** is a character that a person controls with a keyboard and mouse,
as against one an AI or a script moves. The engine drives a player's character
from the person's **controls**, the same way whether the game is played alone or
over a network.

## The player blueprint

`blueprints/Player.abp` is a complete player. Place it in a level and press Play:
W, A, S and D walk, the mouse looks around, Space jumps and Left Control
crouches.

It holds two entities:

| Entity | Components |
|---|---|
| `Player` | `Character`, `Player`, `Replicated` |
| `Camera` | `Camera`, `Parent` (the `Player`), `Replicated` |

- **`Character`** is the [character controller](physics-character.md): the
  capsule and how it moves.
- **`Player`** marks the character as one a person controls. A character
  without it is never driven by the keyboard, so put it on your player and leave
  it off your NPCs.
- **`Replicated`** sends both entities to the other machines in a
  [networked game](multiplayer.md). It does nothing in a game played alone.

The blueprint lists the systems that drive it, so any level it is placed in runs
them:

| System | What it does |
|---|---|
| `LocalPlayer` | Decides which player is this machine's: every `Player` in a game played alone, the one this machine controls in a networked game. |
| `PlayerGather` | Fills in this machine's player's controls from the keyboard and mouse. |
| `CharacterCommand` | Turns each player's controls into where its character faces and what it is asked to do. |
| `PlayerCamera` | Makes this machine's player's camera the active one. |
| `CharacterEye` | Keeps the camera at the character's eye height. |

## A player's controls

The controls are a component, `PlayerInput`, which the engine adds to each
player that needs one:

| Field | Meaning |
|---|---|
| `move` | Where the player is asking to go, in the character's own frame: `x` to the right, `y` forward. |
| `lookYaw`, `lookPitch` | How far to turn left and look up since the last fixed step, in radians. |
| `held` | The buttons held now. |
| `pressed` | The buttons pressed since the last fixed step. |

Each button is a **player action**. The engine has two of its own,
`PlayerAction::Jump` and `PlayerAction::Crouch`, which read the input actions
`Jump` and `Crouch` from `assets/config/input.json` (see [Input](input.md)).
Check one like this:

```cpp
#include <Assisi/App/PlayerComponents.hpp>

if (controls.held.Has(Assisi::App::PlayerAction::Crouch)) { /* held down now */ }
if (controls.pressed.Has(Assisi::App::PlayerAction::Jump)) { /* pressed since the last step */ }
```

The machine whose player it is fills `PlayerInput` in. In a networked game the
host fills in a remote player's from what that player's machine sends, so the
same code reads every player's controls on the host.

The `LocalPlayer` component marks this machine's player, for a system of your
own that should only act on it, such as a heads-up display.

## Your own actions

There are 32 player actions. The engine uses the first two; the other 30 belong
to your game. Take one with `GameAction` and give it the name of the input
action that presses it, in a header under your game's `src/`:

```cpp
#include <Assisi/App/PlayerComponents.hpp>

inline constexpr Assisi::App::PlayerAction Aim = Assisi::App::GameAction(0);

inline constexpr Assisi::App::PlayerActionName kPlayerActions[] = {
    {"Aim", Aim},
};
ASSISI_PLAYER_ACTIONS(kPlayerActions);
```

`Aim` is then held while the input action `Aim` is, and a system reads it like
the engine's own:

```cpp
ASYSTEM(FixedUpdate, name = "Aiming", before = "CharacterCommand")
void AimingSystem(Assisi::App::SystemContext &ctx)
{
    for (auto [entity, controls] : ctx.world.scene.Query<Assisi::App::PlayerInput>())
    {
        const bool aiming = controls.held.Has(Aim);
        // ...
    }
}
```

The build fails if two actions share a slot or a name, or if `GameAction` is
given a number past the last slot.

Read controls in a **`FixedUpdate`** system that runs `before = "CharacterCommand"`.
`CharacterCommand` clears `pressed` and the turning once the step has used them,
so a press is seen by exactly one step, and a system after it would never see
one.
