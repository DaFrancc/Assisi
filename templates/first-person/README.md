# First-person template

A complete first-person player for an Assisi game: it walks, runs, sprints,
crouches and jumps, looks around with the mouse, and its body animates for
everyone else to see, alone or online. Copy it into your game and change it as
you like. Your copy is yours, and the engine never changes it.

The book's [Players](../../book/src/players.md) chapter explains each part.

## What's here

- `src/Player/`: the player's systems and components.
  - `Look`: turns the character and tilts its camera, and draws the view
    following the mouse every frame.
  - `Movement`: turns the controls into where the character goes.
  - `CameraRig`: keeps the camera at eye height and draws through this
    machine's player's camera.
  - `BodyAnimation`: feeds the body's animator, and plays a gesture when your
    game asks.
  - `PlayerActions`: the Walk button, and where your own buttons go.
- `assets/`: the player's blueprint, its animator and the blend space it walks
  on, laid out as they go in your game's `assets/`.

Every file opens with a comment saying what it is for and what to change.

## Starting a project from it

1. Copy `src/Player/` into your game's `src/`.
2. Copy everything under `assets/` into your game's `assets/`, keeping the
   folders.
3. Add the Walk button to the `actions` in your `assets/config/input.json`:

   ```json
   "Walk": ["LeftAlt"]
   ```

4. Get the character model. The body is the `UAL1` character from Quaternius's
   Universal Animation Library, a free download from Quaternius that isn't part
   of Assisi or this template:
   1. Put `UAL1.glb` in `assets/quaternius/UAL1/`.
   2. In the editor's asset browser, right-click it and choose
      **Extract animations**.
   3. Point the template at your imported files. The blueprint and the blend
      space name the model and its clips by the asset ids they had on the
      machine the template was made on, and your import gives them new ones:
      - In `blueprints/Player.abp`, set the `Body` entity's `MeshRenderer`
        `mesh` to `quaternius/UAL1/UAL1.glb`.
      - In `animation/Locomotion.ablnd`, set each of the four points' `Clip`
        to the extracted clip of the same name, in
        `quaternius/UAL1/UAL1_animations/`.

      The animator, `animation/Player.sgl`, names its clips by path, so it
      needs nothing.

   Until the model is there, the body is drawn as the ERROR sign, and
   everything else works. This is a manual step for now: the model is a
   stand-in, and once it is replaced with a model of our own, the template
   will carry everything it needs.
5. Place `blueprints/Player.abp` in a level, or give a `PlayerStart` that
   blueprint for a networked game.

## In Assisi itself

Assisi's sample game plays this template: `apps/game` builds `src/` from here,
and keeps copies of `assets/` in its own `assets/`. A test fails if any file
here and its copy differ. When you change one, copy it over the other.
