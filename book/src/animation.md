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
   Each animation becomes its own file beside the model, named
   `<model>_<animation>.glb`, such as `UAL1_Walk_Loop.glb`. These are ordinary
   glTF files that open in Blender.
3. Give your character entity an `AnimationPlayer`, and set its `clip` to one of
   those files.
4. Add the `AnimationPlayers` system to the level. It is what plays them.
5. Press Play. The clip plays while the game runs, not while you edit.

`AnimationPlayer` has three settings:

| Setting | What it does |
|---|---|
| `clip` | The clip file to play. |
| `speed` | 1 plays at the speed the clip was made at, 2 twice as fast, a negative number backwards. |
| `loop` | On, the clip starts again from the top when it ends. Off, it stops on its last frame. |

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
