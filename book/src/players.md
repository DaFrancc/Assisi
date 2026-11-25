# Players

A **player** is a character that a person controls with a keyboard and mouse,
as against one an AI or a script moves.

The engine gives every game the parts it can't do without:
- which character is the player on this machine
- the player's **controls**, read from the keyboard and mouse
- the [character controller](physics-character.md) that moves the capsule
- the cursor

What a game does with the controls, such as turning, walking, which camera to
look through and how the body animates, is the game's own code.

You don't have to write that code from nothing. The **first-person template**,
in `templates/first-person/`, is a complete first-person player you copy into
your game and then change as you like. Assisi's sample game plays it.

## The first-person template

| File | What it does |
|---|---|
| `src/Player/Look.hpp`, `Look.cpp` | Turns the character left and right and tilts its camera up and down, and draws the view following the mouse every frame. |
| `src/Player/Movement.hpp`, `Movement.cpp` | Turns the controls into where the character goes, and whether it jumps, crouches or sprints. Holding Walk walks instead of jogging. |
| `src/Player/CameraRig.hpp`, `CameraRig.cpp` | Keeps the camera at the character's eye height, and makes this machine's player's camera the one drawn. |
| `src/Player/BodyAnimation.hpp`, `BodyAnimation.cpp` | Feeds the body's animator from what the character does, and plays a gesture when your game asks. |
| `src/Player/PlayerActions.hpp` | The Walk button, and where your own buttons go. |
| `assets/blueprints/Player.abp` | The player: the character, its camera and its body. |
| `assets/animation/Player.sgl`, `Locomotion.ablnd` | The body's animator and the blend space it walks and runs on. |
| `README.md` | What to copy where. |

Every file opens with a comment saying what it is for and what to change.

### Starting a project from it

1. Copy `templates/first-person/src/Player/` into your game's `src/`, so you
   have `src/Player/Look.hpp` and the rest. The game's build picks up every file
   under `src/`.
2. Copy everything under `templates/first-person/assets/` into your game's
   `assets/`, keeping the folders.
3. Add the Walk button to the `actions` in your `assets/config/input.json`:

   ```json
   "Walk": ["LeftAlt"]
   ```

4. Get the character model. The player's body is the `UAL1` character from
   Quaternius's **Universal Animation Library**, which is free to download from
   Quaternius but isn't part of the engine or the template. Put `UAL1.glb` in
   `assets/quaternius/UAL1/`. Then, in the asset browser, right-click it and
   choose **Extract animations** (see [Playing a clip](animation.md#playing-a-clip)).

   Then point the template at what you imported. The blueprint and the blend
   space name the model and its clips by the ids they had where the template
   was made, and your import gives them new ones:
   - In `blueprints/Player.abp`, set the `Body` entity's `MeshRenderer` `mesh`
     to `quaternius/UAL1/UAL1.glb`.
   - In `animation/Locomotion.ablnd`, set each point's `Clip` to the extracted
     clip of the same name.

   The animator names its clips by path and needs nothing. Until the model is
   set up, the body is drawn as the
   [ERROR sign](rendering-models.md#the-error-sign) and everything else works.
   This is a manual step for now: the model is a stand-in, and once we replace
   it with one of our own, the template will carry everything it needs.
5. Place `blueprints/Player.abp` in a level, or for a networked game, give a
   `PlayerStart` that blueprint (see [Players](multiplayer.md#players)).

Press Play: the mouse is captured as soon as the level has loaded, W, A, S and
D walk, Left Alt held walks slowly, the mouse looks around, Space jumps, Left
Control crouches and Left Shift sprints.

From then on the copy is yours. The engine never changes your copy when the
template changes.

## The player blueprint

`blueprints/Player.abp` holds three entities:

| Entity | Components |
|---|---|
| `Player` | `Character`, `CharacterLocomotion`, `ArcadeMovement`, `Player`, `Look`, `Movement`, `ViewRig`, `Replicated` |
| `Camera` | `Camera`, `Parent` (the `Player`), `Replicated` |
| `Body` | `MeshRenderer`, `SkinnedMesh`, `AnimationPlayer`, `Animator`, `Parent` (the `Player`), `Replicated` |

- **`Character`**, **`CharacterLocomotion`** and **`ArcadeMovement`** are the
  [character controller](physics-character.md): the capsule, how fast it walks
  and jumps, and how it speeds up and slows down. Swap `ArcadeMovement` for
  `SourceMovement` to move like Half-Life 2 instead.
- **`Player`** marks the character as one a person controls. A character
  without it is never given controls, so put it on your player and leave it off
  your NPCs.
- **`Look`**, **`Movement`** and **`ViewRig`** are the template's own:
  - `Look` sets how far up and down the view may tilt.
  - `Movement` sets how much Walk slows a full move.
  - `ViewRig` names the camera that tilts and sits at eye height.
- **`Body`** is what other players see: an animated character, standing in the
  capsule and facing where the player faces. Its `Animator` runs
  `animation/Player.sgl`, which walks, runs, crouches, jumps and lands as the
  character does (see
  [Players' bodies animate on every machine](multiplayer.md#players-bodies-animate-on-every-machine)).
  Its `MeshRenderer` has **`hiddenFromOwnCamera`** set, so it isn't drawn
  through a camera from the same blueprint. You don't see the inside of your
  own body, but everyone else sees you, and your shadow is still cast. To use
  your own model, set the `MeshRenderer`'s `mesh` to it and the `Animator`'s
  `machine` to a `.sgl` file written for its skeleton.
- **`Replicated`** sends the entities to the other machines in a
  [networked game](multiplayer.md). It does nothing in a game played alone.

The blueprint lists the systems that drive it, so any level it is placed in runs
them:

| System | Whose | What it does |
|---|---|---|
| `LocalPlayer` | Engine | Decides which player is this machine's: every `Player` in a game played alone, the one this machine controls in a networked game. |
| `PlayerGather` | Engine | Fills in this machine's player's controls from the keyboard and mouse, in `PreUpdate`, so the frame's fixed steps use what this frame read. |
| `Look` | Template | Turns the character and tilts its camera, in `FixedUpdate`. |
| `LookAhead` | Template | Draws this machine's player's view turned as far as the mouse has moved, every frame. |
| `Movement` | Template | Fills in the character's `CharacterIntent` from the controls, in `FixedUpdate`. |
| `EyeHeight` | Template | Keeps the camera at the character's eye height. |
| `PlayerView` | Template | Makes this machine's player's camera the active one. |
| `BodyAnimation` | Template | Sets each body's animator params from what its character does. |
| `Animators`, `AnimationPlayers` | Engine | Run each body's animator and play what it chooses (see [Animators](animator.md)). |
| `CaptureCursor` | Engine | Captures the mouse once the level has loaded (see [Capturing the cursor](input.md#capturing-the-cursor)). |
| `CursorToggle` | Engine | Captures the mouse again on a left click after a menu let it go. |

The sample game also has `blueprints/TestPlayer.abp`, the same player without
`CaptureCursor` and `CursorToggle`, so the cursor stays free. Its test levels
use it, for example to run several players in one editor without every window
grabbing the pointer.

## A player's controls

The controls are a component, `PlayerInput`, which the engine adds to each
player that needs one:

| Field | Meaning |
|---|---|
| `move` | Where the player is asking to go, in the character's own frame: `x` to the right, `y` forward. |
| `lookYaw`, `lookPitch` | How far to turn left and look up since the last fixed step, in radians. |
| `held` | The buttons held now. |
| `pressed` | The buttons pressed since the last fixed step. |

Each button is a **player action**. The engine has three of its own,
`PlayerAction::Jump`, `PlayerAction::Crouch` and `PlayerAction::Sprint`, which
read the input actions `Jump`, `Crouch` and `Sprint` from
`assets/config/input.json` (see [Input](input.md)).
Check one like this:

```cpp
#include <Assisi/App/PlayerComponents.hpp>

if (controls.held.Has(Assisi::App::PlayerAction::Crouch)) { /* held down now */ }
if (controls.pressed.Has(Assisi::App::PlayerAction::Jump)) { /* pressed since the last step */ }
```

The machine whose player it is fills `PlayerInput` in. In a networked game the
host fills in a remote player's from what that player's machine sends, so the
same code reads every player's controls on the host.

After every system of a fixed step has run, the engine clears the turning and
the presses, so every system of the step sees the same ones and a press is seen
by exactly one step. What is held stays: it is what the keyboard says now.

The `LocalPlayer` component marks this machine's player, for a system of your
own that should only act on it, such as a heads-up display.

When a character becomes this machine's player, the engine pushes a
`LocalPlayerGained` event naming it. When it stops being one, because control
moved elsewhere or it was destroyed, the engine pushes `LocalPlayerLost`. Read
them to make and remove things a player has one of, such as its HUD (see
[Showing values from the world](ui-bindings.md)), rather than checking every
frame whether a player has one yet:

```cpp
for (const Assisi::App::LocalPlayerGained &gained : ctx.events.Read<Assisi::App::LocalPlayerGained>())
{
    // gained.entity is the character
}
```

When the character was destroyed, the entity the event names is already gone,
and so are its components. `Scene::IsAlive` tells the two cases apart. A screen
the character owned can't be found from it any more, so have the screen say
which one it is (see
[Screens an entity owns](ui-run-time.md#screens-an-entity-owns)).

### Looking ahead of the step

Turning is applied by the fixed step, 60 times a second. A view turned only
there would turn in steps on a faster screen. The template's `LookAhead`
draws your player's camera turned on by the turning the step hasn't used yet,
every frame, by setting its `DrawPose` (see
[Moving the view every frame](rendering-camera.md#moving-the-view-every-frame)).
Only the drawing turns, along with anything attached to the camera, such as a
held weapon. Where the character faces, what the host is sent, and what
everyone else sees all come from the step.

## Your own actions

There are 32 player actions. The engine uses the first three; the other 29 belong
to your game. The template's `src/Player/PlayerActions.hpp` takes the first of
them for Walk. Add yours beside it, each with the name of the input action that
presses it:

```cpp
inline constexpr Assisi::App::PlayerAction Walk = Assisi::App::GameAction(0);
inline constexpr Assisi::App::PlayerAction Aim = Assisi::App::GameAction(1);

inline constexpr Assisi::App::PlayerActionName kPlayerActions[] = {
    {"Walk", Walk},
    {"Aim", Aim},
};
ASSISI_PLAYER_ACTIONS(kPlayerActions);
```

`Aim` is then held while the input action `Aim` is, and a system reads it like
the engine's own:

```cpp
ASYSTEM(FixedUpdate, name = "Aiming", predicted, after = "LocalPlayer")
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

Read controls in a **`FixedUpdate`** or **`PostFixedUpdate`** system that runs
`after = "LocalPlayer"`, so a character that has just become this machine's
player has its controls before the step that first reads them.

In a networked game a client sometimes runs a step again, to correct what it
predicted. A system that changes what the player's machine predicts, as
`Look` and `Movement` do, is marked `predicted` so that it runs again too (see
[Your systems in multiplayer](multiplayer.md#your-systems-in-multiplayer)).
