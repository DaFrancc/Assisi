# Collision from a model

A box collider is fine for a crate, but a rock or a chair wrapped in a box
collides in the wrong places: objects stop in mid-air beside it, or a chair
cannot be pushed under a table. A `Collider` can take its shape from the model
instead.

## Using a model's shape

Set the `Collider`'s `shape` to `Convex` or `Mesh`, and its `collisionAsset` to
the model: the same `.glb` or `.gltf` file a `MeshRenderer` draws.

| `shape` | The collision shape | Can be used on |
|---|---|---|
| `Convex` | The model's **convex hull**: the tightest shape with no dents or holes that wraps the whole model, like shrink-wrap. If the model contains collision pieces (see below), those are used instead. | Any body, including dynamic ones. |
| `Mesh` | The model's exact triangles, dents and holes included. | Static and kinematic bodies only. |

Start with `Convex`. A hull fills in every dent: a mug's hull is solid, so
nothing can go inside it. When that matters, add collision pieces to the model,
as the next section shows.

## Adding collision pieces to a model

To give a model a shape with dents, such as a chair you can push under a table,
build its collision out of several simple pieces. Model them in your modelling
tool (for example Blender), in the same file as the model, as extra objects. The
start of each object's name says what kind of piece it becomes:

| Name starts with | The piece is |
|---|---|
| `UCX_` | A convex hull of the object: the object's shape with any dents filled in. |
| `UBX_` | A box that fits around the object. |
| `USP_` | A sphere that fits around the object. |
| `UCP_` | A capsule along the object's longest side. |
| `UCY_` | A cylinder along the object's longest side. |

Objects with these names are only collision: the engine does not draw them.
The names are the ones Unreal Engine uses, so a model made for Unreal works
unchanged. `UCY_` is Assisi's own. Upper or lower case both work.

A chair might contain:

```
Chair         the chair you see
UCX_Seat      a block shaped around the seat
UCX_Back      a block shaped around the backrest
UCP_Leg_1     a capsule around each leg
UCP_Leg_2
...
```

Things to know when modelling the pieces:

- **Keep each `UCX_` piece free of dents.** The hull fills them in anyway. To
  collide with a concave shape, split it into several convex pieces.
- **Separate parts become separate pieces.** If one object holds two blocks
  that do not touch, each block becomes its own piece, named `UCX_Seat_1`,
  `UCX_Seat_2` and so on. Blocks that touch, even at one corner, stay one piece.
- **Boxes, spheres, capsules and cylinders are fitted for you.** Model the
  object roughly the right size and place; the engine fits the shape around it
  and places it where the object is.
- **A flat `UCX_` piece is an error.** A single plane has no inside, so the
  import fails and the error names the piece.
- **Use few pieces.** Each piece costs time every time something is near the
  object. A model with more than 32 pieces logs a warning.

## Choosing between hull, pieces and mesh

| You have | Use |
|---|---|
| A moving object without dents: a barrel, a rock | `Convex`, with no collision pieces in the model. |
| A moving object with dents or gaps: a chair, a mug, a table | `Convex`, with collision pieces in the model. |
| Level geometry that never moves, or that your code moves: terrain, buildings, a lift | `Mesh`. |

A `Mesh` cannot be used on a dynamic body: a surface of triangles has no inside,
so the engine cannot work out how it falls and tumbles. If you try, the engine
logs an error naming the entity and uses the model's `Convex` shape instead.

## Many copies of one model

Every collider that names the same model, with the same `shape`,
`collisionPiece` and `density`, shares one collision shape, at any scale. A
level with three hundred chairs reads the chair's collision once, while the
level loads.

A hit on a model's collision reports the entity with the `Collider`, not which
piece was hit: a ray that hits a chair says it hit the chair, not which leg.
When you need to know the piece, use a collider blueprint, below.

## Collider blueprints: one child entity per piece

Sometimes gameplay needs the pieces to be separate: a shot that breaks off the
chair leg it hit, or a lid that can be switched off. For that, turn the model's
collision pieces into child entities, one `Collider` each, saved together as a
blueprint (a reusable group of entities; see
[Levels and blueprints](levels-and-blueprints.md)).

To make one, select an entity whose `Collider` is `Convex` or `Mesh` and names
the model, and press **Make collider blueprint** under the collider's fields.
You can also right-click the model in the asset browser and choose **Make
collider blueprint**. Both write the blueprint next to the model file:
`chair.abp` for `chair.glb`.

The blueprint contains:

- **A root entity** with a `MeshRenderer` that draws the model. It has no
  `RigidBody`; add one if the object should move. The children then become its
  pieces, as described in [Colliders on child entities](physics-child-colliders.md).
- **One child per collision piece**, named after the piece and placed where it
  is in the model. A box, sphere, capsule or cylinder piece becomes a `Collider`
  of that shape. A `UCX_` piece becomes a `Convex` `Collider` that names the
  model and the piece's number in `collisionPiece`.

Place the blueprint in a level like any other. To turn a placed copy into
ordinary entities that are no longer linked to the file, call
`ExplodeInstance` from code; see [Levels and blueprints](levels-and-blueprints.md).

The blueprint is a copy taken when you make it. If you change the model's
collision afterwards, make the blueprint again; the editor asks before
replacing the old file.

The button is greyed out for a model whose collision is a single piece, or the
hull of the whole model: there is only one collider, so there is nothing to
split. Hovering the button says so.

Which to use:

| | The model's collision | A collider blueprint |
|---|---|---|
| A hit reports | The entity with the `Collider` | The child entity of the piece that was hit |
| Pieces can be moved or switched off on their own | No | Yes |
| Updates when the model's collision changes | Yes | No, make it again |
| Cost | One entity | One entity per piece |

Use the model's collision for objects that appear many times, and a collider
blueprint when gameplay needs to tell the pieces apart.

## Model collision in the editor

- Hover a model in the asset browser to see what collision it contains, for
  example "Collision: 3 hulls, 4 capsules".
- The editor draws the outline of a model's collision, as it does for boxes and
  spheres, so you can check it matches the model.
- After saving a changed model from your modelling tool, press **Reimport** in
  the asset browser so the editor reads its collision again.
