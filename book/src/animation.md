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
while it fades out, so a walk doesn't freeze halfway through a step.

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

## Example: turning a joint from code

The pose is a list with one entry per joint. Each entry is the joint's position,
rotation and scale relative to its parent joint, so turning a shoulder turns the
whole arm.

A playing clip writes the joints it moves every Update, so code that adjusts one
of them runs afterwards, in PostUpdate. Joints the clip doesn't move are yours in
any stage.

```cpp
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Runtime/Components.hpp>

ASYSTEM(PostUpdate, name = "LookLeft") void LookLeftSystem(Assisi::App::SystemContext &ctx)
{
    using namespace Assisi;

    for (auto [entity, skinned, renderer] :
         ctx.world.scene.Query<Mut<Runtime::SkinnedMesh>, Runtime::MeshRenderer>())
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
- Write to `pose` only. `SkinnedMesh` also holds `jointModel` (where each joint
  ends up, relative to the entity) and `palette` (what the renderer uses to move
  the vertices). The engine works both out from `pose` every frame, in the
  editor too, so anything you write there is replaced.

The mesh bends on screen in the same frame you write the pose, and so does its
shadow.

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
