# Sigil

**Sigil** (Simple Graph Language) is a small text language for describing
**state machines**: a set of **states** something can be in, and the
**transitions** that move it from one state to another when a condition becomes
true. A character's animation is one: standing, running, jumping and falling
are states, and "start falling when not on the ground" is a transition.

Sigil files end in `.sgl`. You write them in a text editor, and the cook turns
them into a compact form the game runs.

Sigil only *decides*. It reads values your C++ code writes, such as the
character's speed or whether it is on the ground, and picks a state. Anything
that needs real computation, like a raycast or a timer that game rules care
about, stays in C++ and reaches Sigil as one of those values. So Sigil has no
loops, no functions of your own and no variables it changes.

> **Status:** this page describes the core language. Nothing in the engine
> reads `.sgl` files yet: the animation state machine is the first thing that
> will. Until then, the words used for animation below (`layer`, `state`,
> `play`, `fade` and so on) are examples, and may change.

## Example: a character that walks, jumps and falls

```
use animation;

// Values the game writes every frame.
param speed: float;
param grounded: bool;
param jump: trigger;

// Fixed for this file.
const walk_clip = "UAL1_animations/Walk_Loop.glb";
const jump_clip = "UAL1_animations/Jump_Start.glb";
const fall_clip = "UAL1_animations/Jump_Loop.glb";

layer base {
    state walk { play walk_clip; }
    state jump_start { play jump_clip; then fall; }
    state fall { play fall_clip; }

    walk -> jump_start when jump && grounded { fade 0.05; };
    any - jump_start -> fall when !grounded { fade 0.15; };
    fall -> walk when grounded { fade 0.1; };
}
```

Read it top to bottom:

- `use animation;` says this file is written for the animation system. Every
  file starts by naming the system it is for, and that system decides which
  words like `layer`, `state` and `play` exist.
- `param` lines are the file's **inputs**. The game writes them; the file only
  reads them.
- `const` lines name values that never change.
- `layer base { ... }` holds three states. The character starts in the first
  one, `walk`.
- Each line with `->` is a transition: "from this state, go to that one
  when this condition is true".

## Try it: check a file with sglc

`sglc` is the Sigil compiler on its own, as a command-line tool. It reads `.sgl`
files and prints what's wrong with them, without running the game or the cook.
Until the animation system arrives, it only knows a small made-up vocabulary
called `robot`, which is enough to try every part of the language on this page.

### Step 1: build it

`make gd` builds it with everything else. The tool ends up at
`out/build/gcc-debug/apps/sglc/sglc`.

### Step 2: check the example

```
out/build/gcc-debug/apps/sglc/sglc --root apps/sglc/examples apps/sglc/examples/arm.sgl
```

It prints `Compiled "apps/sglc/examples/arm.sgl"`. `--root` is the folder that
`import` paths start from, like the `assets/` folder for the cook.

### Step 3: see the errors

Run it on `apps/sglc/examples/broken.sgl`, the same file with mistakes in it.
Each mistake is listed with the line it's on and the spot underlined; the last
line says `could not compile` and how many errors there were. In a terminal the
output is in color; add `--color=never` to turn that off, or `--color=always` to
keep it when piping to a file.

`sglc` exits with 0 when every file compiles, 1 when any has errors, and 2 when
it couldn't read a file or the command was wrong, so a script can use it as a
check.

The `robot` vocabulary has machines (`machine`) holding nodes (`node`), which can
hold nodes of their own. A node can `emit "beep";` (or `"boop"`, `"whirr"`),
`goto` another node, and have a `cost` in whole numbers; a machine has a `speed`;
a transition can wait `after` some seconds. Its functions are `battery()` and
`tick()`, and `tick()` counts as a trigger.

## How a file is laid out

- **The first statement is `use <system>;`,** naming the system the file is for.
  Comments may come before it.
- **Every statement ends with `;`,** transitions included, even when they end
  in braces. Blocks like `state walk { ... }` end with `}` and need no `;` after
  it. Line breaks don't matter, so a long statement can be split over several
  lines.
- **Comments** are `//` to the end of the line, or anything between `/*` and
  `*/`.
- **Names** are made of letters, digits and `_`, and don't start with a digit.
  Two things declared in one file can't have the same name. States are the one
  exception, explained under [Blocks and states](#blocks-and-states).
- **Some words are reserved** and can't be used as names: `use`, `sigiltype`,
  `import`, `param`, `const`, `let`, `enum`, `when`, `any`, `true`, `false` and
  the type names, plus the words of the system named on the `use` line.
- **Text in double quotes** is a string, used for file paths and other names
  that come from your assets: `"UAL1_animations/Walk_Loop.glb"`. Inside one,
  write `\"` for a quote and `\\` for a backslash. A name without quotes always
  means something declared in the file.

## Types

Every value in Sigil has a **type**, and the cook refuses a file that mixes them
up.

| Type | Holds | Written as |
|---|---|---|
| `float` | a number with a fractional part | `2.0`, `0.15` |
| `int` | a whole number | `2`, `-3` |
| `bool` | true or false | `true`, `false` |
| `trigger` | true for one frame, then false again by itself | (set by the game) |
| an enum you declare | one of a fixed list of names | `Stance.crouched` |

An `int` can be used where a `float` is wanted, so `speed > 2` works when
`speed` is a float. A `float` is never turned into an `int` for you.

The system a file is for can add types of its own. Animation, for example, adds
`clip`, for a path to a clip or blend space file.

## Inputs, constants and formulas

There are three kinds of named value:

**`param name: type;`** declares an input that the game writes:

```
param speed: float;
param run_clip: clip;
```

A param of a type like `clip` is filled in on the component that uses the file,
so one file can serve many characters, each with its own clips.

**`const name = value;`** names a value the cook works out once. It may only use
numbers, strings, enum values and other consts:

```
const walk_speed = 1.5;
const run_speed = walk_speed * 3;
```

**`let name = formula;`** names a formula worked out again every frame from
params, consts and earlier lets:

```
let moving = speed > walk_speed;
let fast_turn = abs(turn) > 0.5;
```

A `let` never stores anything: using `moving` is the same as writing
`speed > walk_speed` in its place. Nothing in Sigil can change a value after it
is declared.

You can write the type of a const or let to make it clearer, and the cook checks
it matches: `let moving: bool = speed > walk_speed;`.

**Order matters for consts and lets:** each one may only use consts and lets
declared above it. Params and enums can be used anywhere in the file.

## Enums

An **enum** is a type you declare, with a fixed list of values:

```
enum Stance { standing, crouched, prone }

param stance: Stance;
```

Its values are written with the enum's name in front: `Stance.crouched`. The
cook catches a misspelt value, and refuses comparing a `Stance` with a number or
with another enum.

## Expressions

Conditions and formulas are built from these operators, loosest first:

| Operators | Meaning |
|---|---|
| `\|\|` | or |
| `&&` | and |
| `==` `!=` | equal, not equal |
| `<` `<=` `>` `>=` | comparisons, for numbers |
| `+` `-` | add, subtract |
| `*` `/` `%` | multiply, divide, remainder (`%` for ints only) |
| `!` `-` | not, negative |

Use parentheses to group: `(a || b) && c`.

Dividing one `int` by another gives a whole number: `7 / 2` is `3`. Write
`7.0 / 2` for `3.5`.

**Functions** are written `name(...)`:

| Function | Gives |
|---|---|
| `abs(x)` | `x` without its sign |
| `min(a, b)`, `max(a, b)` | the smaller or larger of two numbers |
| `clamp(x, low, high)` | `x` kept between `low` and `high` |

The system a file is for can add functions of its own, such as animation's
`progress()`, how far the current state's clip has played from 0 to 1. Every
function only reads; none changes anything.

## Blocks and states

A **block** is a kind, a name, and a body in braces: `layer base { ... }`,
`state walk { ... }`. Which kinds exist, and which may go inside which, is up to
the system the file is for.

Inside a block, a **clause** is a word followed by its values and a `;`, such
as `play walk_clip;`. The system says which clauses a block may hold and what
type each value must be.

The blocks inside a block are its **states**:

- **The first state written is where the block starts.**
- **State names only need to be different within their block,** so two layers
  can both have a state called `idle`.
- **States can hold states of their own**, where the system allows it: a
  `combat` state could hold `ready` and `swing`, with transitions between them.
  The rules on this page then apply inside it as well.
- **States and transitions can come in any order** inside a block, and a
  transition can name a state written further down.

## Transitions

A transition says: from these states, go to that state when this condition is
true.

```
walk -> jump_start when jump && grounded { fade 0.05; };
```

- **`when` and a condition are always required.** The condition must be a
  `bool`.
- **Clauses go in braces after the condition,** each ending with `;`, just like
  clauses in a block. The transition still ends with `;`, after the `}`:
  ```
  fall -> walk when grounded;
  any - jump_start -> fall when !grounded {
      fade 0.15;
      interrupt;
  };
  ```
- **The first true transition wins.** Transitions are checked in the order
  they're written, so put the one that matters most first.

**More than one source state.** The part before `->` can name several states:

| Written | Means |
|---|---|
| `walk -> fall` | from `walk` |
| `walk + run -> fall` | from `walk` or `run` |
| `any -> fall` | from every state in the block except `fall` |
| `any - jump_start -> fall` | from every state except `fall` and `jump_start` |

`+` adds a state and `-` removes one, read left to right.

**Restarting a state.** `any` never includes the state it leads to, so a
transition only restarts a state when you write that state on both sides:

```
flinch -> flinch when hit { fade 0.05; };   // hit again during a flinch: start it over
```

## Triggers

A `trigger` is for something that happens at a moment rather than a condition
that holds, such as a button press or a hit landing. The game sets it, and it is
true for that one frame only.

- **A transition that fires uses up the triggers its condition read,** so for
  the rest of that frame they read false. One press can't fire two transitions.
  A transition that reads a trigger but doesn't fire leaves it alone.
- **A trigger nothing used is gone the next frame.** It isn't saved for later.
  If a press should still count a moment afterwards, for example jump pressed
  just before landing, have your C++ code keep setting the trigger for a few
  frames.
- **Triggers only decide transitions.** A trigger can be used in a `when`, or in
  a `let` that is only used in `when`s. Anywhere else, like a clause value, the
  cook refuses it.

## Sharing values between files

Enums and consts that several files need can live in a **library**: a file
marked `sigiltype library;` right after its `use` line.

```
// shared/movement.sgl
use animation;
sigiltype library;

enum Stance { standing, crouched }
const walk_speed = 1.5;
```

```
// characters/knight.sgl
use animation;
import "shared/movement.sgl";

param stance: Stance;
let moving = speed > walk_speed;
```

- **A library may hold only imports, enums and consts.**
- **Only libraries can be imported.** An import brings in the library's enums
  and consts, and nothing else.
- **Names can't clash.** An imported name that matches one in your file is an
  error.

## What the cook checks

The cook refuses a file with a mistake in it. For each mistake it says what's
wrong, then where, as `file:line:column`, then shows the line with the spot
underlined with `^` and a few words beside it. Other places that explain the
mistake are underlined with `-`, such as where a name was first declared. A
misspelt name gets the closest real one suggested, and when there's more to say
about the fix, a `help:` line follows:

```
error: unknown state "fal" in layer "base"
  --> characters/knight.sgl:14:13
   |
14 |     walk -> fal when !grounded;
   |             ^^^ did you mean "fall"?

error: this transition goes to "flinch", so it can't also leave from "flinch"
  --> characters/knight.sgl:16:5
   |
16 |     any + flinch -> flinch when hit;
   |     ^^^^^^^^^^^^    ------ and it goes to "flinch"
   |     |
   |     these include "flinch"
   |
   = help: to restart "flinch" while it's playing, write "flinch -> flinch" as a transition of its own
```

Mistakes are listed from the top of the file down. One mistake isn't reported
again at every place it shows up later: a name the cook refused once is left
alone after that.

Among other things, it refuses:

- a value of the wrong type, such as `when speed` (a float is not a condition);
- a name nothing declares, or one declared twice;
- a const that reads a param (use `let`);
- a const or let that uses one declared below it;
- a state that no transition can ever reach;
- removing a state from a set it isn't in, as in `walk - run -> fall`;
- a trigger used outside a `when`.

It also warns, without refusing the file, about a param, const, let, enum or
import that nothing uses. Libraries don't get these warnings, and neither does a
file with mistakes in it, because a part the cook couldn't check may be what
uses them.
