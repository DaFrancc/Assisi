# Animation

A character bends at its elbows and knees because its mesh is **skinned**: it
comes with a **skeleton**, a tree of **joints** (also called bones), and every
vertex of the mesh is attached to one or more joints. Move a joint and the
vertices attached to it move with it.

The way every joint is placed at one moment is called a **pose**. The pose a
model was built in, before anything moves it, is its **rest pose**.

## Using a rigged model

1. Export the model from your modelling tool as `.gltf` or `.glb`, with its
   armature (Blender's name for a skeleton). The engine reads the skeleton and
   which joints each vertex follows.
2. In the editor, give an entity a `MeshRenderer` whose `mesh` is that file.
3. Add a `SkinnedMesh` to the same entity. It starts in the model's rest pose.

The editor draws the skeleton over the model, in blue, or in the selection
colours when the entity is selected. Each bone is a long diamond: thick at the
joint it hangs from and pointed at the joint it reaches, so you can tell which
way it goes. Each joint also has a small cross. Hover over a joint to see its name. The **Skeletons** checkbox in the
F11 options panel turns this off; like collider outlines, it is hidden while
the game is playing.

`SkinnedMesh` has nothing to edit in the inspector. It is saved with the level
so the entity stays skinned, but the pose itself is not saved: it is set again
every time the level loads.

Things to know about the model:

- **One skeleton per file.** A file with two skeletons, or with a skinned mesh
  and a mesh that has no skeleton, is refused. Put a prop that isn't part of
  the character in its own file.
- **At most four joints move each vertex.** A vertex attached to more keeps the
  four that pull it hardest, and the import log says so once.
- **Joints need different names.** Animations find joints by name, so two
  joints with the same name are refused.

## Playing a clip

A **clip** is one recorded movement, such as a walk or a jump. Tools usually
export a character's clips inside its model file, often dozens in one file. The
engine wants one clip per file, so you take them out first:

1. Put the model file, with its animations, in `assets/`.
2. In the asset browser, right-click it and choose **Extract animations**.
   Each animation becomes its own file in a folder beside the model, named
   `<model>_animations`: `UAL1.glb`'s walk becomes
   `UAL1_animations/Walk_Loop.glb`. These are ordinary glTF files that open in
   Blender.
3. Give your character entity an `AnimationPlayer`, and set its `animation` to
   one of those files.
4. Add the `AnimationPlayers` system to the level. It is what plays them.
5. Press Play. The clip plays while the game runs, not while you edit.

`AnimationPlayer` has these settings:

| Setting | What it does |
|---|---|
| `animation` | The clip file to play, or a blend space (see [Blend spaces](#blend-spaces)). |
| `parameter` | Where in a blend space to play. A clip ignores it. |
| `speed` | 1 plays at the speed the clip was made at, 2 twice as fast, a negative number backwards. |
| `loop` | On, the clip starts again from the top when it ends. Off, it stops on its last frame. |
| `fade` | Seconds to fade over when `animation` changes, 0.15 unless you change it. 0 switches at once. |
| `layers` | Other animations played on top (see [Layers](#layers)). |

Things to know about clips:

- **Clips find joints by name.** A clip plays on any model whose joints have the
  same names as the joints it moves, so one walk works for every character built
  on the same skeleton. Joints the clip names that the model lacks are skipped,
  and the log says which, once per character.
- **Extracting again updates the clips.** After re-exporting the model, extract
  again: the clip files are rewritten and everything using them keeps working. A
  clip renamed in your tool becomes a new file, and a clip you deleted leaves its
  old file behind for you to delete.
- **Only Extract animations writes clip files.** Importing a model never does,
  so a model's animations take no space until you ask for them.
- **Some animations can't play yet.** Curves stored as cubic splines, and
  animations of blend shapes (morph targets), are left out when you extract, with
  a warning naming them. Re-export with linear keys, which Blender does by
  default, and leave blend shapes out.

## Fading from one animation to another

Switching straight from a walk to a jump makes the character jump into the new
pose in a single frame. A **cross-fade** hides that: for a moment both play, and
the pose slides from one to the other.

Change `animation` from your code, and the player's `fade` sets how long the
slide takes: 0.15 seconds unless you change it. The old animation keeps moving
while it fades out, so a walk doesn't freeze halfway through a step. It keeps
the `speed` and `loop` it was playing with, too: a looping walk fading into a
still pose, or into an animation that plays once, goes on walking until its fade
ends.

- **Changing again during a fade is safe.** The new fade starts from the pose on
  screen at that moment, so nothing jumps.
- **A fade runs in real seconds.** `speed` doesn't make it faster or slower, so a
  fade still finishes while `speed` is 0.
- **Joints the new animation doesn't move fade back to the rest pose.**
- **Clearing `animation` returns the character to its rest pose**, over `fade`
  seconds like any other change. After that the pose is yours to set from code.

## Blend spaces

A **blend space** mixes several clips at once, chosen by a number your code sets.
Picture idle, walk and run placed along a line by speed:

```
idle      walk           run
 0 ------- 2 ------------ 6   speed
```

At speed 2 the character walks; at 4 it is half walking and half running, which
looks like a jog. Raise the speed smoothly and it eases from standing to walking
to running.

The clips in a space share one timeline, measured from the start of a cycle to
its end rather than in seconds, so a walk and a run of different lengths put the
same foot down at the same moment and the feet don't slide while they mix. The
cost is that a clip plays a little faster or slower than it was made while it is
mixed with a longer or shorter one.

A blend space is a file of its own, ending in `.ablnd`, that lists each clip and
where it sits. There is no editor for it yet, so you write it by hand. Create
`assets/characters/Locomotion.ablnd`:

```json
{
  "version": 1,
  "type": "BlendSpace",
  "Points": [
    { "Clip": { "guid": "<Idle_Loop.glb's guid>" }, "Position": [0, 0] },
    { "Clip": { "guid": "<Walk_Loop.glb's guid>" }, "Position": [2, 0] },
    { "Clip": { "guid": "<Jog_Fwd_Loop.glb's guid>" }, "Position": [6, 0] }
  ]
}
```

Each clip's guid is the `guid` line in its `.aast` sidecar file, next to the
clip. Then set the player's `animation` to the space, and each frame write the
character's speed into the first number of `parameter`:

```cpp
player.parameter.x = glm::length(velocity);
```

A space can also spread clips over a flat area, using both numbers of
`Position` and `parameter`. A strafing character puts a run forward at `[0, 1]`,
backward at `[0, -1]`, left at `[-1, 0]` and right at `[1, 0]`, and sets
`parameter` to its velocity in its own facing: moving forward and right at once
mixes the forward and right runs into a diagonal.

Things to know about blend spaces:

- **One space serves every character on the same skeleton.**
- **A space waits for all its clips.** Nothing plays until every clip it lists
  has loaded.
- **Past the last point, the last point plays.** A speed of 9 in the space above
  plays the run as it is.
- **The cook refuses a space that can't play**: one with no points, a point with
  no clip, or two points at the same position.

## Layers

A character often does two things at once: runs with its legs while its arms
aim, or flinches when hit whatever else it's doing. **Layers** play other
animations on top of the player's own, which is called the **base**.

`AnimationPlayer` has a `layers` list. In the inspector, open it and click
**+ Add** for each layer. Each one has the base's settings (`animation`,
`parameter`, `speed`, `loop`, `fade`) and these:

| Setting | What it does |
|---|---|
| `mode` | `Override` replaces the pose below with the layer's; `Additive` adds the layer's movement on top. |
| `maskRoot` | The joint the layer works on, with every joint below it. Empty means the whole skeleton. |
| `exclusions` | Joints left out of the mask, each with every joint below it. |
| `weight` | How much the layer counts, from 0 (off) to 1 (fully on). |
| `weightFade` | Seconds the weight takes to reach a new value; 0 changes it at once. |
| `weightSlide` | `Duration`: every change takes `weightFade` seconds. `Speed`: the weight moves 1 every `weightFade` seconds, so a small change is quick. |

Layers apply in the order of the list, each on top of the result so far.

### Example: run, aim and flinch

1. Set the player's `animation` to a run.
2. Add a layer with an aim clip, `mode` `Override` and `maskRoot` the first
   joint of the spine, such as `spine_01`. To keep the head looking where the
   run looks, add the neck to `exclusions`.
3. Add a second layer with a flinch clip and `mode` `Additive`, with `maskRoot`
   left empty.

The legs run, the upper body aims, and the flinch jolts both. Set the flinch
layer's `weight` to 0 until the character is hit.

### Override and additive

- **An override layer only replaces what its animation moves.** A joint in the
  mask that its clip has no keys for keeps what the layers below give it, so
  an aim clip that doesn't move the fingers leaves them to the run.
- **An additive layer adds how far its animation has moved since its first
  frame.** A flinch recorded from a standing pose works as it is: standing
  adds nothing, and only the jolt is added.
- **Order matters: put override layers above additive ones.** A flinch below
  the aim layer jolts the aiming arms. Above it, the aim replaces the flinch on
  the joints its clip moves, but joints it doesn't move, often the hands and
  fingers, keep the jolt while their parents aim, so they come out of line.

Things to know about layers:

- **A layer's animation keeps playing at weight 0**, so turning a layer back on
  doesn't restart it.
- **Moving a layer up or down the list doesn't restart it.** Its animation
  carries on where it was; only the order it applies in changes.
- **Changing a layer's animation fades over that layer's own `fade`**, the same
  way the base fades.
- **A mask joint the model lacks is logged once.** A `maskRoot` it lacks makes
  the layer move nothing; an exclusion it lacks is skipped.
- **Joints a layer's mask takes are written every Update.** Code that adjusts
  one of them runs after the `AnimationPlayers` system, as for joints the base
  clip moves.

## Choosing animations with a state machine

Setting `animation`, `fade` and the layers from code works for one or two
animations. A character that walks, runs, jumps, lands and aims needs rules for
which plays when, and an **Animator** runs those rules from a `.sgl` file. With
an Animator on the entity, it decides what the player plays every frame, and
the player's own `animation`, `parameter`, `speed`, `loop`, `fade` and
`layers` are not used. They keep whatever you set, so change the file rather
than those fields. [Animators](animator.md) shows how.

## Example: turning a joint from code

The pose is a list with one entry per joint. Each entry is the joint's position,
rotation and scale relative to its parent joint, so turning a shoulder turns the
whole arm.

A playing clip writes the joints it moves every Update, in the `AnimationPlayers`
system, so code that adjusts one of them runs after that system. Joints the clip
doesn't move are yours in any stage.

```cpp
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Runtime/Components.hpp>

ASYSTEM(Update, name = "LookLeft", after = "AnimationPlayers")
void LookLeftSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;

    for (auto [entity, skinned, renderer] :
         ctx.world.scene.Query<Mut<Runtime::SkinnedPose>, Runtime::MeshRenderer>())
    {
        // Empty until the mesh has loaded.
        if (skinned.pose.empty())
        {
            continue;
        }
        const int32_t head = Geometry::FindJoint(renderer.meshBuffer->Skeleton(), "Head");
        if (head == Geometry::kNoJoint)
        {
            continue;
        }
        skinned.pose[static_cast<size_t>(head)].Rotation = glm::angleAxis(0.5f, glm::vec3(0.f, 1.f, 0.f));
    }
}
```

- `FindJoint` gives a joint's position in the list, or `kNoJoint` when the
  skeleton has no joint with that name.
- A joint keeps the value you give it until something changes it again. It goes
  back to the rest pose only when the entity's mesh changes, or its
  `AnimationPlayer` starts another clip.
- The pose lives in `SkinnedPose`, which the engine adds next to every
  `SkinnedMesh`. It is never saved and never sent over the network: each machine
  works out its own.
- Write to `pose` only. `SkinnedPose` also holds `finalPose`, `jointModel`
  (where each joint ends up, relative to the entity) and `palette` (what the
  renderer uses to move the vertices). The engine works all three out from
  `pose` every frame, in the editor too, so anything you write there is
  replaced.

The mesh bends on screen in the same frame you write the pose, and so does its
shadow.

### When a pose is finished

Each frame, once every `Update` system has run, the engine finishes every
character's pose in one step, the **pose stage**:

1. It places every entity in the world.
2. It copies each `pose` into `finalPose`.
3. It works out `jointModel` and `palette` from `finalPose`.
4. It moves everything held at a joint to where that joint now is (see
   [Holding something at a joint](#holding-something-at-a-joint)).
5. It places again whatever moved since step 1.

`finalPose` is kept apart from `pose` for steps that adjust a pose after the
animation, such as turning a head to look at something. They change only
`finalPose`, so each frame they start again from the animation, and a change
never builds up from frame to frame. The engine has no such steps yet, so for
now `finalPose` always matches `pose`.

So what you read from `jointModel` depends on when your system runs:

| Your system runs in | `jointModel` is from |
|---|---|
| `Ui` | This frame's pose. |
| `Update`, `FixedUpdate` | Last frame's pose, the one on screen now. |

The pose stage runs in every program: the game, the editor (also while you
edit, or while the game is paused) and a dedicated server. A dedicated server
loads no meshes, so nothing is posed there.

## Holding something at a joint

A sword in a hand or a hat on a head has to move with the joint it's on, not
just with the character. To make an entity follow a joint:

1. Make the sword its own entity, with a `Transform` and a `MeshRenderer`.
2. Give it a `Parent` whose `parent` is the character, the entity with the
   `SkinnedMesh`.
3. Add a `JointAttachment` and type the joint's name into `joint`, for example
   `hand_r`. To find a joint's name, select the character in the editor and
   hover over the joint in the skeleton drawn over it.
4. Set the sword's `Transform` to where it should sit relative to the joint:
   in the palm rather than at the wrist, turned to point forward.

From then on the sword follows the hand, and anything parented to the sword
follows it too. The `Transform` stays the offset you set, measured from the
joint. Moving the sword with the gizmo in the editor changes that offset, so
you can line it up by eye.

- **It never trails the hand.** The pose stage moves held things after the
  joints are final, in the same frame, so the sword isn't a frame behind.
- **The joint is found by name.** If you re-export the model with its joints
  in another order, or swap the character's mesh for one with a joint of the
  same name, the sword stays on that joint.
- **A name the mesh doesn't have** puts a warning in the log, naming the
  joint, and the sword hangs from the character's origin instead.
- **Only the joint's name and the offset are saved.** Where the joint is at
  this moment is worked out again every frame on every machine. It is never
  saved in the level and never sent over the network.
- **On a dedicated server, held things stay at the character's origin,**
  because nothing is posed there.

### Picking something up while the game runs

`levels/PickUp.alvl` has a sphere on the floor. Walk into it and it goes into
your right hand. Press **Q** to drop it in front of you, then walk away and
back to pick it up again. Pick **Host + 2 clients** and watch another window's
player: your own body is hidden from your camera.

The code is in `apps/game/src/Pickup.cpp`. In a networked game, add the
`Parent` and the `JointAttachment` on every machine, not only on the host,
because parent links are not sent over the network:

1. The sphere has a replicated `Holdable` component whose `holder` names the
   character holding it, or nobody.
2. Only the host changes `holder`: when a player walks into the sphere, or
   asks to drop it.
3. Every machine runs `HoldHoldables`, which gives a held sphere a `Parent`
   (the holder's body) and a `JointAttachment`, and takes both off a free one.

A player who joins later gets `holder` with everything else, so they see the
sphere in the right hand too.

## Animation in a multiplayer game

Animation is not sent over the network. When a character arrives on another
machine, its `SkinnedMesh`, `AnimationPlayer` and `Animator` arrive with it, and
that machine plays the animation itself. Two machines can be a moment apart, but
what players see stays close.

So game code that plays animation runs on every machine, not only the host:

- A system that sets an Animator's params, or changes a player's `animation`,
  should run everywhere, from state every machine has, such as a character's
  `CharacterState`.
- Changing a player's or an Animator's own fields on the host sends the change
  to everyone, like any other replicated field. Do it when something changes,
  not every frame.
- Params, the pose and where each clip has got to are never sent. Setting a
  param every frame costs nothing on the network.

## How a pose reaches the screen

Each frame, before anything is drawn, the GPU moves every skinned entity's
vertices into its pose. Each entity gets its own copy of its mesh's vertices for
this, so two characters using the same model can stand in different poses. From
there on a posed character is drawn, culled and shadowed like any other mesh.

What that means for you:

- **Each character costs memory for its vertices.** A 10,000-vertex character
  takes about half a megabyte for its posed copy. When a character is removed,
  the next character with the same model reuses its copy.
- **Culling follows the pose.** When an arm reaches out past the edge of the
  screen, the character is still drawn while the arm is in view.
- **A character that keeps changing pose keeps its shadow redrawn.** A
  character in a pose that doesn't change for a while has its shadow kept, like
  any object that has stopped moving.

## Measuring animation

The game's [benchmark](rendering-debugging.md#benchmarking-a-level-along-a-camera-route)
also reports what animation costs the processor each frame. Run it on any level
with animated characters. `levels/AnimationCrowd.alvl` is made for it: 100
copies of the UAL1 character from the Quaternius Universal Animation Library,
each walking with a blend space, aiming with its upper body, and holding an
additive layer at weight 0. Put the character in `assets/quaternius/UAL1/`
first; the level needs it.

```bash
./assisi package ship --profiler
cd out/build/gcc-ship-chiara/apps/game
./Assisi-Game --benchmark levels/AnimationCrowd.alvl
```

When the run ends, the log shows milliseconds per frame for each step:

| Step | What it is |
|---|---|
| `animators` | Running each Animator's state machine. |
| `playback` | Moving clips on and blending them into each pose. |
| `transforms` | Placing every entity in the world, animated or not, before the poses. |
| `poses` | Turning each pose into the joint positions the renderer moves vertices by. |
| `transforms after poses` | Placing again what the poses moved. Only what changed is visited, so this should stay small. |

Below that it says how many characters it measured, with how many joints and
layers each, and the cost per character and per joint. **The numbers depend on
the content.** A skeleton with more joints, clips with more keys, or more layers
cost more. Use the per-joint cost to estimate other characters, and compare
runs on the same level to see whether a change made animation faster or slower.
Measure with a `ship` build that has the profiler, never `dev` or `debug`.
