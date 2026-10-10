# Animators

An **Animator** picks a character's animations for you. You write the rules in
a `.sgl` file: which **states** the character can be in, such as walking or
jumping, what each state plays, and the **transitions** that move it from one
state to another, such as "start the jump when the jump button is pressed".
This is called a **state machine**. Each frame the Animator checks the rules
and tells the entity's [AnimationPlayer](animation.md#playing-a-clip) what to
play, with a cross-fade whenever it changes.

The rules only decide. Your C++ code tells the Animator what is going on, such
as how fast the character moves or whether it is on the ground, through values
called **params**.

`.sgl` files are written in Sigil. This page teaches the parts you need for
animation; [Sigil](sigil.md) covers the whole language.

## Example: a character that walks, runs, jumps and aims

This file uses the clips extracted from Quaternius's Universal Animation
Library, `UAL1.glb`, and the `Locomotion.ablnd` blend space from
[Blend spaces](animation.md#blend-spaces), which mixes idle, walk, jog and
sprint by speed:

```
use animation;
skeleton "quaternius/UAL1/UAL1.glb";

param speed: float;
param grounded: bool;
param jump: trigger;
param aiming: float;

const locomotion_space = "quaternius/UAL1/Locomotion.ablnd";
const jump_start_clip = "quaternius/UAL1/UAL1_animations/Jump_Start.glb";
const fall_clip = "quaternius/UAL1/UAL1_animations/Jump_Loop.glb";
const land_clip = "quaternius/UAL1/UAL1_animations/Jump_Land.glb";
const aim_clip = "quaternius/UAL1/UAL1_animations/Pistol_Aim_Neutral.glb";

layer base {
    state locomotion { play locomotion_space; x speed; }
    state airborne {
        state jump_start { play jump_start_clip; then fall; }
        state fall { play fall_clip; }
    }
    state land { play land_clip; then locomotion; }

    locomotion -> airborne when jump && grounded { fade 0.05; };
    airborne -> land when grounded { fade 0.1; };
    land -> airborne when !grounded { fade 0.15; interrupt; };
}

layer upper {
    mask "spine_01";
    weight aiming;
    state aim { pose aim_clip; }
}
```

Read it top to bottom:

- `use animation;` says the file is an animation state machine.
- `skeleton "...";` names the model whose joints the file is written for. The
  cook checks every joint the file names against it.
- `param` lines are the values your code sets: `speed` and `aiming` are
  numbers, `grounded` is true or false, and `jump` is a **trigger**, which is
  true for one frame when your code sets it.
- `const` lines give names to file paths, so the states below read easily.
  Paths start at the `assets/` folder.
- `layer base` holds the states for the whole body. The character starts in
  the first state written, `locomotion`, which plays the blend space at the
  place `speed` puts it: standing at 0, walking at 1.5, sprinting at 6.
- `airborne` holds two states of its own. Entering it starts `jump_start`,
  which plays once and then moves on to `fall` by itself.
- The `->` lines are the transitions. `locomotion -> airborne when jump &&
  grounded` jumps when the trigger is set and the character is on the ground,
  fading into the jump over 0.05 seconds.
- `layer upper` plays on top of the base, on the joint `spine_01` and every
  joint below it: the spine, arms and head. It holds the aiming pose, as much
  as `aiming` says, from 0 (not at all) to 1 (fully).

Save it as `assets/quaternius/UAL1/Character.sgl`.

## Try it: put the character in a level

### Step 1: add the components

In the editor, give an entity a `MeshRenderer` with the mesh
`quaternius/UAL1/UAL1.glb`, and add a `SkinnedMesh`, an `AnimationPlayer` and
an `Animator`. Set the Animator's `machine` to `Character.sgl`.

### Step 2: add the systems

Add these systems to the level:

| System | What it does |
|---|---|
| `AnimatorKeys` | Sets the params from the keyboard and mouse, for trying a file out. |
| `Animators` | Runs every Animator's file and tells its player what to play. |
| `AnimationPlayers` | Plays what each player was told. |

`AnimatorKeys` is a test system from the game's own code. A real game sets the
params from its own systems instead (see
[Setting params from code](#setting-params-from-code)).

### Step 3: play

Press Play. The character stands still. Hold W to walk, and add Shift to
sprint. Press Space to jump, and keep holding it to stay in the air; let go to
land. Hold the right mouse button to raise the pistol while doing any of it.

### Step 4: change the file while it runs

Leave the game playing and change `fade 0.1;` on the landing transition to
`fade 1;` in `Character.sgl`, then save it. Within half a second the editor
loads the new file; the next landing fades over a whole second. The character
keeps doing what it was doing: an Animator stays in the state of the same name
when its file changes.

## Layers

Each `layer` plays its own states at the same time as the others. The first
layer plays on the whole body. Each layer after it plays on top, like the
player's [layers](animation.md#layers). These clauses go in a layer, before its
states:

| Clause | What it does |
|---|---|
| `mask "joint";` | The layer moves only that joint and every joint below it. |
| `exclude "joint";` | Leaves that joint, and every joint below it, out of the mask. Write one per joint. |
| `additive;` | Adds the layer's movement to the pose below instead of replacing it. |
| `weight <number>;` | How much the layer counts, from 0 to 1. Worked out every frame, so it can be a param. |
| `fade <seconds>;` | How long the layer's transitions fade when they don't say. 0.15 unless you change it. |

The first layer is the whole body under every other, so it can't have `mask`,
`exclude`, `additive` or `weight`; it can have `fade`.

## States

A state either holds other states or plays something. One that plays has
exactly one of these:

| Clause | What it plays |
|---|---|
| `play <clip>;` | A clip file or a blend space (`.ablnd`), from the start. It loops, unless the state has `then`. |
| `pose <clip>;` | The clip's first frame, held still. Not a blend space. |

And any of these:

| Clause | What it does |
|---|---|
| `x <number>;` | Where in a blend space to play, along its first axis. Worked out every frame. |
| `y <number>;` | The same along the second axis, for a blend space spread over an area. |
| `rate <number>;` | How fast to play: 1 as authored, 2 twice as fast. Worked out every frame. |
| `then <state>;` | Plays the clip once, then moves on to that state by itself. The state must be one of this one's neighbours. |

A value worked out every frame can be a number, a param, or a formula of them,
such as `rate speed / 1.5;`.

### States inside states

A state can hold states of its own, like `airborne` above. This groups states
that share their way out: one transition from `airborne` to `land` covers both
`jump_start` and `fall`.

- **Entering a state that holds states enters the first one inside it**, and
  so on down to a state that plays something.
- **A state that holds states plays nothing itself**, so it has none of the
  clauses above.
- **Outer transitions are checked first.** `airborne -> land` fires as soon as
  the character is grounded, whichever state inside `airborne` it is in, even
  if `jump_start` was about to move on to `fall`.
- **A transition connects states written side by side**, in the same block.
  To go somewhere inside a state from outside, go to the state, which starts at
  its first.

## Transitions

A transition is written inside the layer or state that holds both its states:

```
locomotion -> airborne when jump && grounded { fade 0.05; };
```

- **The first true transition, in the order written, fires.** Outer blocks are
  checked before inner ones.
- **`fade <seconds>;`** sets how long the change fades. Without it, the layer's
  `fade` is used.
- **A transition waits while a fade is still running**, so a quick flicker of a
  param can't jerk the character back and forth. Add **`interrupt;`** to let it
  fire during a fade, as `land -> airborne` does: walking off a ledge mid-landing
  should fall at once.
- **A trigger is used up by the transition that reads it.** A trigger no
  transition used is gone the next frame.
- **`progress()`** is how far the current state's clip has played, from 0 to 1,
  for conditions such as `when progress() > 0.8`.

`any`, `+` and `-` choose several states at once, as described in
[Transitions](sigil.md#transitions).

## Setting params from code

A system sets the params before the Animators system runs, by name:

```cpp
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/AnimatorStep.hpp>
#include <Assisi/Runtime/Components.hpp>

ASYSTEM(Update, name = "HeroAnimation", before = "Animators")
void HeroAnimationSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;
    for (auto [entity, animator, state, intent] :
         ctx.world.scene.Query<Mut<Runtime::Animator>, Physics::CharacterState, Physics::CharacterIntent>())
    {
        const glm::vec2 across{state.velocity.x, state.velocity.z};
        Runtime::SetAnimatorFloat(animator, "speed", glm::length(across));
        Runtime::SetAnimatorBool(animator, "grounded", state.ground == Physics::GroundState::OnGround);
        if (intent.jump)
        {
            Runtime::FireAnimatorTrigger(animator, "jump");
        }
    }
}
```

This one reads a [character controller](physics-character.md): its speed across
the ground, whether it stands on something, and whether it was asked to jump.

| Function | Sets |
|---|---|
| `SetAnimatorFloat` | A `float` param. |
| `SetAnimatorInt` | An `int` param, or an enum param by its value's position. |
| `SetAnimatorBool` | A `bool` param. |
| `FireAnimatorTrigger` | A `trigger` param, for this frame. |
| `AnimatorFloat` | Reads a `float` param back, for easing one towards a target. |

The setters return false and change nothing when the file has no param of that
name and type, or hasn't loaded yet; `AnimatorFloat` returns nothing then. A
param keeps its value until you set it again, except a trigger.

## One file for many characters

A `clip` param lets each character fill in its own clip:

```
param run_clip: clip;

layer base {
    state run { play run_clip; }
}
```

The Animator's `clips` list says which clip each one is: add a row, set `name`
to `run_clip` and `clip` to the file. A param with no row plays nothing, and
the log says so once.

## What the cook refuses

A mistake fails the cook, with the file, the line and the spot underlined, so
it never reaches the game. Among others:

- a clip or blend space path that isn't in `assets/`, or a path to something
  that isn't one;
- a `skeleton` that isn't a model, a file without one, and a `mask` or
  `exclude` joint the model doesn't have, with the closest real name suggested;
- a state that plays nothing, or plays two things;
- a clause on a state that holds states;
- `pose` of a blend space, and `then` on a `pose`, which never finishes;
- `mask`, `exclude`, `additive` or `weight` on the first layer.

The editor shows the same messages in its log when it loads a file with
mistakes, and keeps the character on the last version that worked.
