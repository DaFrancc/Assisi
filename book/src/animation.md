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

## Example: turning a joint from code

The pose is a list with one entry per joint. Each entry is the joint's position,
rotation and scale relative to its parent joint, so turning a shoulder turns the
whole arm.

```cpp
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Runtime/Components.hpp>

void LookLeftSystem(Assisi::App::SystemContext &ctx)
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
  back to the rest pose only if the entity's mesh changes.
- Write to `pose` only. `SkinnedMesh` also holds `jointModel` (where each joint
  ends up, relative to the entity) and `palette` (what the renderer uses to move
  the vertices). The engine works both out from `pose` every frame, in the
  editor too, so anything you write there is replaced.

The mesh does not bend on screen yet. The pose is worked out and kept up to
date, but drawing a skinned mesh in its pose is still to come; until then it is
drawn in its rest pose.
