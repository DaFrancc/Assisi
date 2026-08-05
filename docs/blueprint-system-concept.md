# Blueprints and levels — the concept

What the system is for and what it has to do, with none of the mechanism.
This is the brief for building it.

There was a first attempt — the whole thing, built in a day and then set
aside. It is not in this tree. It lives on the tag
**`archive/blueprints-attempt-1`**, along with its 1174-line design record
(`docs/blueprint-design-notes.md`), which has the file format, the review
dispositions and the engine surveys if any of that is ever wanted:

    git show archive/blueprints-attempt-1:docs/blueprint-design-notes.md

That attempt's *concept* was mostly right and is kept below. Its shape was
not: it treated blueprints as a second kind of thing beside levels, gave
instances a runtime identity built on a synthetic root, and parented members
to that root. All three are reversed here.

## Why

Levels today author every component inline. The tenth bouncy cube copies
nine components again, and fixing its restitution edits ten levels. Worse,
the ECS split between data and code fails silently: a component whose system
was never installed just does nothing, with no error and nothing to
diagnose.

So: **most level content should be instances of other content.** A level
lists, per instance, which file and what that instance changed — nothing
else. Levels stay small, a fix propagates on next load, and the behaviour a
piece of content needs travels with it instead of depending on the level's
profile happening to install the right systems.

---

## 1. Levels and blueprints are the same file

A level today is three keys: `version`, an optional `profile`, and
`entities`. A blueprint would have been the same list with a shorter
`entities` array. They are not two formats; they were never going to be.

So there is **one format, one loader, one editor**. Authoring a blueprint is
opening a file. "Create blueprint from selection" is "save the selection as
a file, and replace it with an instance of that file."

This is Godot's model, where a `.tscn` is a scene and a prefab and there was
never a distinction. Unreal kept them apart for a decade and then added
Level Instances anyway. Nobody converges the other direction.

### Two extensions, two verbs

`.alvl` and `.abp` are the **same format**. The extension is a hint to
humans and something for the asset browser to filter on; it never gates
behaviour.

What actually differs is the **verb**:

| | Load | Instance |
|---|---|---|
| the world | replaced | added to |
| placement | none — it *is* the world | a transform |
| `systems` | installed | installed |

Instancing a `.alvl` into another `.alvl` works, because it has to — the
loader cannot tell them apart, and a distinction enforced weakly is worse
than one enforced not at all. The editor may note it; nothing refuses it.

### The file

```json
{
  "version": 2,
  "systems":  [ "Bounce", "InputDemo" ],
  "entities": [ { "name": "body", "components": { ... } } ],
  "instances": [
    { "name": "car_3", "source": "content/car.abp",
      "transform": { ... },
      "overrides": { ... },
      "removed":   [ "wheel_fl" ] }
  ]
}
```

---

## 2. What survives into the game

**Nothing survives on the entities except a tag** saying which instance each
one belongs to.

```cpp
ACOMP(replicable) struct BlueprintMember
{
    AFIELD() uint32_t instanceId;    // which spawned copy
    AFIELD() uint32_t memberIndex;   // which entry in the blueprint's member list
};
```

Eight bytes, and **no name on the entity**. Names are paths once nesting is
involved — `parking_lot_a/car_3/wheel_fl` is 26 characters — so a fixed
32-byte inline string overflows at two ordinary levels of nesting, and
truncation makes two members indistinguishable. That silently retargets
overrides and `FindMember`, which is the exact failure §6's naming rule
exists to prevent.

The name is recovered by a walk that already exists: instance table (below)
→ source path → the cached member list (§11) → `[memberIndex].name`. This
also makes `FindMember(world, id, "wheel_fl")` *faster* — the name resolves
to an index once, and the query then compares integers instead of strings per
entity.

The tag is therefore only meaningful while the blueprint's prepared form is
cached, which is exactly as long as the level lives; both are discarded
together. One consequence for §10: when a blueprint edit changes its member
list, the in-place diff must remap surviving entities' indices.

Derived at expansion and **never written to a file** — same tier as `NetId`:
created at load, rebuilt on every load, authored nowhere.

**It does replicate**, because membership is state with a current value, and
the house rule is that nothing with a current value becomes an event
(`replication-plan-v4.md` §5). Without it, a host that prunes a member leaves
the client believing the wheel still belongs to the car — and no despawn
record has a correct reading afterwards. `instanceId` is a per-world counter,
so the wire carries the instance's `baseNetId` (§9) and the client maps it to
its own local id on receipt, the same translation `EntityRef` → `NetId`
already performs. It is sent once and never again unless a prune changes it.

There is no root entity, no marker beyond this tag, no lineage tree, and no
stored member list. **"The members of instance 7" is a query**, computed when
asked and discarded. This is deliberate and it is the difference from Unity
DOTS's `LinkedEntityGroup`, which stores the member list on a root entity and
goes stale the moment a member dies or is reparented.

### The instance table

One thing does survive *beside* the scene: a table the world owns, one row
per live instance.

    7  →  content/car.abp
    8  →  content/car.abp
    9  →  content/crate.abp

Not on the entities, not serialized, not replicated. One row per instance,
not per member.

It exists because the tag says *which* instance an entity belongs to and
never *what* that instance is. Without the table, `FindInstance<Car>(world, 7)`
would build a `Car` view over a crate's members and return nonsense, and
"validate an instance against its source" (§5) would have no source to
validate against. The table also holds the root transform, which the editor's
record (§10) needs anyway.

The id allocator lives here too. **Ids are per-world**, handed out from 1
upward, and start over when a level loads, because the table is discarded
with the world. Ids are not stable across a save/load and nothing may assume
they are.

This is the boundary the four failures below actually depend on: not "no
runtime state anywhere", but **nothing on the entities that a system could
branch on**.

### The principle: membership is never load-bearing

**A system reads components. It must never branch on whether an entity came
from a blueprint.**

The test: anything a blueprint produces, code could produce by hand, and
afterwards nothing can tell the difference. A `VehicleDrive` that works on a
placed car must work identically on a car assembled in code and on a car
received over the network.

This is not style. Four concrete failures follow from breaking it:

- A car assembled by hand in code has no instance id, so a system that reads
  one works on placed content and silently does nothing on built content.
- One blueprint can hold two unrelated things (a car *and* its trailer),
  which share an id but are not one vehicle.
- Two blueprints can be one thing (a turret mounted on a tank) — one
  relationship, two ids.
- The tag answers "which instance", never "which *kind* of thing" — so a
  system reading it has learned nothing it can act on without also assuming
  every car came from a blueprint, which is the first failure again.

So the tag is **an argument you hand back to the API, never something a
system searches on** — and the caller must be tooling, not gameplay. The
editor selecting an instance, or a debug command destroying one, reads the
tag legitimately: neither has to work on a hand-assembled car.

A death handler doing `DestroyInstance(member.instanceId)` looks like the
same thing and is not. Reading the tag to get the id *is* branching on
membership: the handler destroys the whole group on a placed car and does
nothing at all on a hand-built one, which is failure #1 above, verbatim. A
death handler wanting to take a vehicle apart reads authored gameplay refs
(below), which work either way.

Iterating `BlueprintMember` to find an entity's siblings is the same smell,
more obviously.

### Relationships live in gameplay components

A wheel must know its chassis — that is real and unavoidable. It is solved by
**wiring members to each other at author time, by name**:

```json
{ "name": "wheel_fl",
  "components": {
    "Parent": { "parent": "body" },
    "Wheel":  { "steers": true, "chassis": "body" }
  } }
```

Every `EntityRef` field may name another member of the same file; expansion
rewrites the name to a real handle, the same rewrite `Parent` gets. After
load these are ordinary `Entity` fields in ordinary components — replicated,
serialized, and working on entities that were never in a blueprint.

That makes a blueprint *a group of entities pre-wired to each other*, which
is the thing that makes them worth having.

## 3. The root is placement, and only placement

An instance needs one thing you can grab to move it. That is the root: a
mesh-less entity with the usual billboard icon, and it **only ever
translates, rotates, or scales uniformly**.

That constraint is load-bearing rather than cosmetic. Uniform scale commutes
with rotation, so composing a parent's transform onto a member's decomposes
cleanly back to TRS. A non-uniform scale anywhere in the chain introduces
shear, and the result cannot be represented as a transform at all.

**The root is not a `Parent`.** It is editor-side grouping — move the root,
every member moves. Members carry no parent component because of it, and the
root does not exist after expansion. It belongs to the *instance*, not to
the file: the same file used as a world root has no root at all.

**Real `Parent` links between members do survive.** Wheels under a body is
genuine hierarchy, not grouping, and it expands into the game like any other
component. Which has a consequence — see §12.

**Uniform scale is enforced on both sides.** The editor cannot author a
violation — an instance's scale is one number in the inspector, not three —
and a hand-edited file carrying a non-uniform instance scale **fails the
load**, naming the file and the instance. Clamping to an axis was rejected:
it lets the file say one thing while the game does another, which is the
failure this document keeps designing away from.

### Which space a transform lives in

Three answers that are obvious once written and get built wrongly if they are
not.

**The component is absolute; the override is relative.** After expansion the
root is gone, so a top-level member's `Transform` is world space — there is
nothing left to be relative to. Members with a real `Parent` are relative to
that parent, as normal. What stays relative is the **override in the file**,
obtained by dividing the instance's placement back out of the edit.

**Moving the instance touches only the record.** Dragging a car updates the
instance entry's transform in the level and records **no member overrides**.
Getting this wrong pins all five members the first time somebody nudges a car.

**The editor shows both, and the gizmo has a frame mode.** A member's
inspector displays its relative transform and its absolute transform; editing
either works. The gizmo's existing world/local toggle
(`EditorGizmo.cpp:123`) becomes three-way for members of an instance:

| mode | axes |
|---|---|
| World | global |
| Local | the entity's own |
| Instance | the blueprint root's frame — handles rotate with the car |

The mode is a **view, not a storage decision**: the override is recorded in
file space regardless, directly in Instance mode and after dividing out the
placement in World mode. ImGuizmo only knows LOCAL and WORLD, so an arbitrary
frame is had by folding the instance transform into the *view* matrix
(`view * instanceRoot`) and passing the member's matrix in instance space.

## 4. Nesting

A file may instance other files. A parking lot is six cars.

- **Cycles hard-fail at load.** `a` containing `b` containing `a` expands
  forever, and it is unrecoverable if missed.
- **The uniform-scale rule applies at every level**, not just the outermost.
- Expansion composes the whole chain — lot placement, car placement, member
  local — and discards every root.

### Nesting flattens at runtime

Spawning `car_with_antenna.abp` — which instances `car.abp` and adds an
antenna — produces **one** instance with **one** id. Members are named by
path:

    antenna
    car/body
    car/wheel_fl

The nested root evaporates exactly as the outer one does. One id means
`DestroyInstance` reaches everything, the subset invariant (§5) still holds,
and validating an instance against its source stays possible.

The alternative — nested instances keeping their own ids — means a tree of
instances at runtime, parent links between them, and every operation having
to ask which level of instance was meant. That is the machinery the
evaporating root exists to avoid.

Flattening is why detaching a nested group is not available; see
*Deliberately omitted*.

## 5. Overrides

A level entry is a source, a placement, a set of overrides, and a removal
list. Nothing else is authored per instance.

- **Recorded, not computed.** An override exists because somebody edited that
  field, not because a comparison found a difference. The computed version
  was built and it was wrong: editing a blueprint while a level was open
  silently froze the old values into every instance as fake overrides.
- **Un-overridden means un-resolved.** A field nobody touched re-reads from
  the source on every load. That is what makes "fix it once, fixed
  everywhere" true, and it is the whole point of the format.
- **Written where the edit was made, addressing downward.** `lot.abp` may
  override `car_3/wheel_fl/…`; a level placing that lot may override
  `lot_a/car_3/wheel_fl/…`. Nothing reaches upward.
- **Outermost wins, per field.** A level's override beats the blueprint's
  for the *same field only*. If a lot sets a wheel's colour and the level
  sets that wheel's radius, both apply. The alternative reading — an outer
  override replacing the whole inner set — is how someone loses edits.
- **Reset is first-class, at every scope**: per field, per component, per
  member, whole instance. Each drops the claim and lets the value fall back,
  and each is one undo entry per gesture rather than one per component it
  happened to touch.

### What an override may change

Fields, and also the *set* of components — an ECS lets components come and go
at runtime, so a file that could not express it would be the odd one out:

```json
"overrides": {
  "body": {
    "MeshRenderer": { "mesh": "meshes/car_red.obj" },   // change fields
    "Turret":       { "range": 40 },                    // add a component
    "RigidBody":    null                                // remove a component
  }
}
```

`null` reads unambiguously as removal, since a real component is always an
object. Because overrides are recorded rather than computed, a removal is a
note saying *this instance does not have it* — so if the blueprint later
drops that component itself, the note becomes a harmless no-op rather than an
error.

Structural divergence like this is legitimate but usually not the best
answer. One `crate.abp` overridden three ways works; three files is often
clearer. The format does not take a side.

### Merging claims across nesting levels

"Outermost wins, per field" is defined for fields. Removal is a
*component-level* claim, so their interaction has to be stated or it will be
decided at the keyboard. The case: `car.abp` declares `RigidBody`, `lot.abp`
removes it with `null`, and a level placing that lot overrides
`RigidBody: { mass: 5 }`.

**Removal wins, and the outer field override is dropped with a warning**
naming the level, the member and the component.

The reasoning is that neither claim can be honoured and the engine cannot
know which one is the mistake — the inner author may have deleted a component
the outer author still needs, or the outer author may be editing something
that no longer exists. Resurrecting the component from a field edit is worse:
it brings back every *other* field of a component somebody deliberately
deleted, silently. So the load succeeds, the component stays gone, and the
warning tells a human to decide whether the removal or the override should
go.

Two smaller rules fall out of the same place:

- **An add starts from the component's C++ defaults**, never from the values
  the blueprint had before an inner level removed it. A re-add is a new
  component that happens to share a name.
- **Two levels adding the same component merge per field**, outermost
  winning per field, exactly as two field overrides do — an add is an object
  claim like any other.

### Instances only shrink

**An override may remove a member. It may never add one.**

Removal is the `"removed"` list on the instance entry — the members are
simply not created. Entries are **downward paths**, identical to override
addresses, so removing a wheel from the third car of a placed parking lot is
`"removed": [ "car_3/wheel_fl" ]`. Nesting the removals inside per-instance
blocks would invent a second addressing scheme for nothing. Addition is refused because an instance's members must
always be a **subset of what its file declares**, and that invariant is what
makes three other things possible: validating an instance against its source,
generating `InstanceView<T>` (§7) from the file alone, and dropping orphaned
overrides safely (§6).

The mechanism for *a car plus an antenna* is nesting: a new file that
instances `car.abp` and adds an antenna. Dropping a loose entity beside the
instance in the level also works — levels hold ordinary entities alongside
instances — but it is not a member, so `DestroyInstance` leaves it behind.

The same invariant holds at runtime: prune and explode remove members;
nothing adds one. See §7.

### There are no runtime overrides

`SpawnBlueprint` takes a source and a transform, and nothing else. A caller
that wants a red car writes the component after spawning:

```cpp
if (auto car = SpawnBlueprint<Car>(world, at))
    world.scene.Get<MeshRenderer>(car->body).mesh = redMesh;
```

Typed, direct, no strings, and it expresses things overrides cannot. Overrides
are an authoring concept for files; there is no file to write at runtime, so
nothing is recorded.

## 6. Entities are named

Entities in a file carry a name, unique within that file. The editor
auto-names on create so nobody thinks about it until they care.

This is forced by overrides. Entities are currently identified by *position*
in the array, which is safe only while nothing outside the file points into
it. An override that says "entity #1" means something different the moment
somebody inserts a member above it — a red wheel silently becomes a red
headlight.

It fixes something latent for free: `Parent` also stores a positional index,
so a hand-edited or merged level file can today re-target a parent link with
no error. Named entities kill that too.

### Names are a contract — no renaming

A member's name is part of the blueprint's public surface, like an inherited
member in an OOP language. **The editor offers no rename**, and an override
naming a member the file does not declare is a **hard error at load**, naming
the file and the member — the same treatment an unknown system name gets
(§8).

Renaming buys nothing and costs the one thing that makes the rest safe.

**Deleting a member is allowed**, and banning renames is what makes it clean:
a missing member can only mean deliberate deletion, so an override addressing
it is **dropped with a log line**, not an error. With renames permitted the
same situation would be ambiguous — deleted, or renamed? — and dropping could
be discarding a real edit.

Adding a member to a blueprint never breaks anything. Deleting one breaks
loudly and locally.

### References are names too

An `EntityRef` field serializes today as a remapped positional index
(`SceneSerializer.hpp:181-184`) — the same fragility, one level down. Files
store names instead, which is a fourth mapping mode beside remap/raw/transfer.

**No migration path.** There are four `.alvl` files in the tree. Convert them
when the format changes and delete the index reader rather than carrying a v1
path forever.

**Into an instance**, a name is a downward path, exactly as an override
address is:

```json
"target": "car_3/body"
```

**Out of an instance**, a leading slash means *the file that wrote this
override*:

```json
"chassis": "body"            // this instance's member
"target":  "/spawn_marker"   // an entity in the file that wrote the override
```

Relative is the default, so nothing reaches outward by accident. The scope is
the writing file rather than "the outermost level", which is what makes it
compose with nesting: `car_with_antenna.abp` overriding its nested `car` to
point at `/antenna` resolves inside `car_with_antenna.abp`. That is §5's
"written where the edit was made", applied to references.

Forbidding outward references was considered and rejected — it would mean a
placed turret could never be wired to a level's marker in the editor, which is
a large hole to leave in nesting. A name that does not resolve becomes null
with a warning, as an orphaned override does.

Doing this with opaque per-entity ids instead (Unity's fileID model) works and
was turned down for the reason §6 opens with: names are already unique per
file, and the readable file is the point. It is moot at runtime either way —
every reference resolves to a real handle at load.

## 7. The runtime API

Four calls, and the game knows about blueprints only through them.

```cpp
SpawnBlueprint(world, source, transform)   // create the group
DestroyInstance(world, id)                 // destroy the group
PruneFromInstance(world, entity)           // one member leaves; entity lives on
ExplodeInstance(world, id)                 // all members leave; the instance ceases to exist
```

Each is thin, none runs per frame, and all of them are engine-provided
precisely because they are what everyone would otherwise hand-roll — usually
as a stored member list.

- `DestroyInstance` scans the `BlueprintMember` pool for the id. A member
  already destroyed simply is not found; there is no list to go stale.
- `Scene::Destroy` is already deferred to `FlushDestroyed`, so calling this
  from inside a system is safe today with no new machinery.
- `PruneFromInstance` removes the tag. `ExplodeInstance` is prune applied to
  every member — which is why explode does not need to be all-or-nothing: a
  partial instance is not a broken state, because membership is a query.
- There is **no adopt**. Entities may be created at runtime freely; they
  never join an instance.

Destruction is by explicit verb. `Scene::Destroy` on a wheel destroys the
wheel and nothing else — no existing destroy path anywhere in the engine
acquires blueprint semantics. This matches how `Runtime::GatherSubtree`
already works: hierarchy-aware destruction is opt-in at the call site.

### What spawning returns

```cpp
std::optional<uint32_t>          SpawnBlueprint(World &, string_view source, const Transform &);
std::optional<InstanceView<T>>   SpawnBlueprint<T>(World &, const Transform &);
std::optional<InstanceView<T>>   FindInstance<T>(World &, uint32_t id);
ECS::Entity                      FindMember(World &, uint32_t id, string_view name);
```

`std::optional`, not a sentinel and not a pointer. A pointer would have to
point at something the engine owns, but a view is built on demand by querying
the tag pool — keeping one alive would invert the receipt rule and leave the
*engine* holding stale handles. And `optional` works with a move-only type,
which the view is.

One failure convention, not two: an earlier draft had spawn return a struct
whose `instanceId == 0` meant failure, which is ignorable in a way
`if (auto car = …)` is not.

`FindInstance` is why the instance table (§2) has to exist — it must confirm
that instance 7 came from `car.abp` before building a `Car` view over its
members, or a crate's members quietly become a car.

**The rule: `instanceId` is what you may keep; the entity handles are for
right now.** An `Entity` goes stale when that entity dies; the id survives
members dying, because `FindMember` re-resolves and `DestroyInstance` finds
whatever is left. Storing handles is how the stored-member-list problem comes
back in disguise.

Returning a `vector<Entity>` was rejected for exactly that reason — it
allocates per spawn, it forces positional indexing, and above all it invites
the caller to hold it.

There is no "primary member" concept. The typed path reaches the body as
`car->body`; the untyped path as `FindMember(world, id, "body")`. A
designated primary would be a third way to say the same thing and a marker in
the file format to maintain.

### `InstanceView<T>` — opt-in codegen

For blueprints stable enough to be part of the code, a generator reads the
file and emits a typed view:

```cpp
if (auto car = SpawnBlueprint<Car>(world, at))
    world.physics.AddForce(car->body, forward * 500.f);
```

A typo does not compile, and renaming a member breaks the build at every call
site — which is not a new tax, because member names are *already* stringly
coupled through `FindMember` and through author-time wiring. Codegen moves an
existing failure from runtime to compile time.

Nesting stays visible in the view even though the runtime is flat:

```cpp
template <> struct InstanceView<CarWithAntenna>
{
    uint32_t    instanceId;
    ECS::Entity antenna;
    struct { ECS::Entity body, wheel_fl, wheel_fr, wheel_rl, wheel_rr; } car;
};
```

`car.body` in code and `car/body` at runtime are the same entity reached two
ways. The nested group carries **no id of its own** — there is one instance.
Grouping rather than flattening to `car_body` avoids a real collision: a
top-level member named `car_body` alongside an instance `car` with a member
`body` would otherwise produce two fields with one name, and break the build
for a reason visible in neither file.

Three rules keep the type from becoming the thing it resembles:

- **Receipts, not references.** An `InstanceView` lives in the scope of the
  call that produced it. Only `instanceId` may outlive it. It is move-only and
  the spawn is `[[nodiscard]]`.
- **Fields only.** The generator emits no methods, and regeneration clobbers
  the file — so `car.ApplyHandbrake()` cannot survive a build. This is closed
  by machinery, not discipline.
- **No reflected component may contain one**, checked by reflectgen. A view
  inside a component is a stored member list plus a serialization hazard.

It is a plain aggregate of handles: no identity, no invariants, no methods, no
polymorphism, and it owns nothing. `Spawned<Car>` was considered and rejected
because spawning is not the only producer — `FindInstance<Car>(world, id)`
returns the same type, so the name must say what it *is*, not how it arrived.
`View` also carries the borrowed-and-temporary connotation of `string_view`
and `span` for free, which is teaching the rule through the name.

`InstanceView<T>` is an undefined primary template specialized by generated
code — the same idiom as `MessageTraits<T>`, so it is the second instance of a
pattern already in the engine rather than a new one.

## 8. Systems

**Profiles are gone.** A file lists the individual systems it needs, by
name — closer to a module import than an include. Longer, and more
straightforward.

Removing them also fixes a misplacement. The sandbox's profile contains
`world.physics.SetContactReporting(true)` — the *level* knowing what
`BounceSystem` needs. That belongs to the system, so that anyone who names it
gets it, and a level that forgot the line stops producing a system that runs
and silently does nothing.

Module semantics, in three parts:

- A name resolves to a catalog entry. An unknown name is a **hard error at
  load**, never a warning.
- Naming a system twice — or two nested blueprints both naming `Bounce` —
  installs it **once**. The list is a union, not a concatenation.
- **File order carries no meaning.** Run order comes from `after`/`before` on
  the system itself, so a level cannot accidentally reorder anything.

### `ASYSTEM`

A system is `(phase, name, function, ordering, scope)`, and data can supply
only the name. So the definitions live in a catalog — and the catalog is
**generated, not hand-written**:

```cpp
ASYSTEM(FixedUpdate)                           void BounceSystem(SystemContext &ctx);
ASYSTEM(Update, activeWorldOnly)               void InputDemoSystem(SystemContext &ctx);
ASYSTEM(Update, name = "Spin", after = Bounce) void SpinDemoSystem(SystemContext &ctx);
```

Phase mandatory and positional, everything else a flag or `key = value` —
the `AMSG` grammar, so there is one thing to learn rather than two.
Annotated free functions only, like `AMSG_HANDLER`, which costs nothing
because the house rule is already that a system keeps its state in
components and never in itself.

This puts phase, ordering and scope *on the function*, three lines above the
code they describe — not in a registration call elsewhere, and not restated
in every level file. It also deletes `registerGameSystems`: linking a
module registers its systems.

Two more rules come with it:

- **Duplicate names, missing `after`/`before` targets, and ordering cycles
  are a whole-tree build error** — the same shape as reflectgen's
  `--check-handlers` pass, reusing that machinery rather than inventing a
  second one.
- **Render phase implies `RenderContext &`**, every other phase
  `SystemContext &`. Codegen enforces it, which is a correctness check the
  manual `Register`/`RegisterRender` split leaves to the caller today.

### No gate

`RequireAny`/`RequireAll` are **not** part of `ASYSTEM`.

A gate is a skip test, not a query — it can only ever be a conservative
superset, exclusions can never contribute to one, and reflectgen cannot
verify it because it reads a declaration and not a function body. Too loose
costs one call; **too tight is a system that silently never runs**, which is
the exact failure this whole document opens with. What it buys is a
function call and an empty query, and a system that needs better than that
can bail early where it has real information — as `BounceSystem` already
does, on an empty contact log.

The existing `SystemRegistry::RequireAny` is a different thing and stays: it
is a *per-frame skip*, costing a couple of array loads, not an installation
decision. It is also most of what uninstalling would have bought.

Recorded so it is a decision rather than an oversight: the case that would
bring an authored gate back is a system whose *emptiness check is itself
expensive*. None exists today.

### Installation is deferred, and there is no uninstall

**Install on demand, applied at a frame safe point.** Loading a level
installs its list. Spawning a blueprint queues the systems it names —
including its nested closure — and they are installed at the `DrainMain` safe
point, where deferred loads and travel already land, *before* the frame's
systems run (`Application.cpp:510-518`). Installing an already-present system
is a no-op.

Not `FlushDestroyed`: that is a different point, at end of frame, and called
per host rather than by the engine. Installs want the one that runs before the
walk over systems begins.

Deferral is required, not preferred: spawning usually happens *inside* a
system, and `SystemRegistry` invalidates its cached execution order on every
registration, so registering mid-walk mutates what is being iterated. The
cost is one frame — the car exists immediately and drives from the next.

**Nothing is ever uninstalled.** The 30-second reclaim sweep is deferred
deliberately, for two reasons: `RequireAny` already makes an idle system cost
a couple of array loads per phase, so there is almost nothing to reclaim; and
loading a level flushes everything anyway, which is the whole answer for a
game that is not open-world. Revisit if streaming ever arrives. The mechanism
it would need is an authored keep-alive on the catalog entry — deriving
"unused" from component presence is the same mistake the authored-list rule
exists to prevent, and would uninstall every system that reads no components
at all.

**The list is authored, never derived.** Tempting to infer it from the
members' components; wrong, and not hypothetically — every blueprint with a
rigid body would declare a dependency on physics when almost none mean it.
The editor may *suggest*; the author decides, and nothing is enforced
afterwards.

## 9. Blueprints across the network

### The hash covers every blueprint

`LevelIdentity.contentHash` today hashes the `.alvl` file's content, and a
client that resolves the path to different bytes refuses the join.

Once a level instances other files, its real content is **the transitive
closure** of everything it instances. So the hash covers all of it: the
level's own content plus every blueprint reachable from it, combined in a
deterministic order (sorted by path — filesystem enumeration order is not
stable across machines). A missing or unreadable file in the closure refuses
the join rather than being skipped.

Hash the **content, not the bytes**. The level hash already had to learn this
(`562aa5d`): a Windows laptop and a Linux desktop refused each other over 439
CR bytes, because git checks text out as CRLF on Windows. Blueprints are JSON
text and inherit the same trap exactly.

**The closure is not enough on its own.** A blueprint spawned from C++ is
named by no level, so it would never be hashed — and the next section makes
every blueprint's content load-bearing across the wire. So the join verifies
**the whole content set**: every `.alvl` and `.abp`, each content-normalised,
combined in sorted virtual-path order into one hash.

**The client sends it; the server decides.** The hash travels in `ClientHello`
and the server refuses a mismatch, naming what differs if it can. The server
owns who joins, which is where a content check belongs, and it matches how the
protocol hash already works.

The check is deliberately strict: *any* difference refuses, including files
neither machine ever loads. A stray experimental `.abp` in the asset folder
means nobody can connect until it is gone. That is a development-time cost and
never a shipping one, and it buys the property the next section depends on —
after a successful join, both machines are known to expand any blueprint
identically.

Because the sets are then guaranteed to match, a blueprint can be named on the
wire by its **index in the sorted list**, two bytes, with no path string and
no per-blueprint hash.

**Hashing is asynchronous.** Reading every content file blocks nothing: it
runs as a job at load, and a client that tries to connect before its hash is
ready joins in appearance only — the connection waits on the hash and
completes when it arrives. The server cannot be reached without one, so there
is no race to lose. The server hashes its own set the same way and refuses
joins until it has.

Shipping eventually replaces the scan entirely: assets bake into a single
archive, so the content set *is* the archive, its hash is one number rather
than a combine over thousands of files, and the stray-file problem cannot
occur. Until then the asset database is the place to cache per-file content
hashes so the join hash is a combine rather than a re-read.

**Unsaved blueprint edits are flushed before hosting.** The manifest is hashed
from disk, so a host that edited `car.abp` and pressed Play would expand from
its in-memory prepared form while clients expand from stale disk — and the
hashes would *match*, because both sides hash disk. A green handshake over two
different worlds is the one failure the manifest exists to prevent, and unlike
the mid-session case it happens in an ordinary workflow. So Play and Host
resolve unsaved blueprint changes first: save, or discard.

Editing a blueprint *during* a live session is left undefined — already-joined
clients keep their old expansion and new joiners refuse on hash mismatch. It
is a thing a developer does deliberately, not a workflow to protect.

Note the narrow scope: this is about `.abp` **contents**. Placing, moving,
overriding, pruning and deleting instances during a session are all fine —
those change the host's entities, which replicate as ordinary state.

### Spawning replicates as a blueprint, not as entities

When the host spawns a blueprint, it sends **the spawn**, not the entities:

    BlueprintSpawn         { blueprintIndex, baseNetId, memberCount, transform }
    BlueprintSpawnFromLevel{ levelInstanceIndex, baseNetId }

The client runs the same expansion from the same file and assigns member *i*
the NetId `baseNetId + i`, which works because expansion order is the file's
order and NetIds for an instance are allocated as one contiguous block.
Twenty-odd bytes instead of every component of every member.

**Two flavours, because a placement is not a blueprint.** The first names a
blueprint and a transform, which is everything a *runtime* spawn has — §5
forbids runtime overrides, so there is nothing else to say. A **level-placed**
instance also carries overrides and removals, and those live in the level
file's instance entry, not in the `.abp`. A client told only *"spawn `car.abp`
at (22,0,4)"* expands a plain car while the host has a red one, and the
divergence is permanent: overrides are applied during expansion, so their
change ticks predate the spawn and the delta path never resends them.

The second flavour names the level's instance entry by its index in file
order. Both machines hold the level file and its hash is already verified, so
source, transform, overrides and removals all come from the same bytes. **No
override vocabulary ever goes on the wire**, which means overrides can grow
arbitrarily complex without touching networking.

One consequence for the join: the strip that removes the client's own copies
of replicated entities must become **instance-aware** — strip whole instances
that contain any replicated member, not just the marked members, or
re-expansion duplicates the instance's non-replicated ones.

Three things fall out of it, and the third is the reason it is not merely an
optimisation:

- **The first snapshot is a delta against the blueprint**, not against an
  empty baseline. Both sides already hold the authored values, so only what
  the host changed after spawning is ever sent.
- **Despawn is one record, not N.** `DestroyInstance` sends the instance, and
  the client destroys the members it built.
- **The client installs the blueprint's systems**, because it genuinely runs
  `SpawnBlueprint`. Sending entity state instead would leave a client holding
  a missile whose trail system was never installed — *"a component whose
  system was never installed just does nothing"*, split across two machines
  and visible only in multiplayer. That hole closes by construction here.

**Relevancy is per instance** for these entities. A blueprint is a spatially
coherent group, so "half a car is relevant" is not a state worth
representing, and instance-granularity means the spawn record is what a
connection receives when the group first becomes relevant to it.

**A client that cannot expand refuses the connection** rather than continuing
without those entities. The join hash makes this unreachable in practice;
treating it as fatal is what keeps it unreachable in principle.

Note what has *not* changed: updates after the spawn are ordinary per-entity
component state, addressed by NetId. Blueprints are how entities come into
existence on a client, not how they are kept in sync — so relevancy, the
acked ring, and delta encoding stay exactly as they are.

### Future: level content stops travelling

`BlueprintSpawnFromLevel` is **provisional**, and whoever builds it should
know why it exists rather than assume it is permanent.

It exists because a joining client currently *strips* every entity carrying
replicated components and receives them back from the host. That strip is not
a safety mechanism — `EditorNet.cpp:176-200` gives the reason plainly: the
level file's copies are the host's authored originals, "so keeping both would
double every replicated object in the world." It is de-duplication, and it is
only necessary because the host resends things the client already built.

The end state is that it does not: **the authored level is a shared baseline
and everything on the wire is a delta against it.** Both machines already load
the same hash-verified file, so NetIds for authored content can be assigned
deterministically at load — instance *i*, member *j* — and the host sends only
what has changed since. Then level instances never travel at all and this
record type is deleted.

That is a rework of the join path affecting every level, with or without
instances, so it belongs to the replication plan rather than here. It is
recorded in `docs/replication-plan-v4.md`, along with what it needs
(deterministic NetIds at load, an explicit destroyed list, seeding the join
from the load tick) and the world-streaming work that motivates it.

## 10. The editor

### Instance identity, and undo

The tag from §2 is what the editor uses: `instanceId` to select and drag an
instance as a unit, `memberIndex` (resolved to a name through the cached
member list) to address an override. It is not transient and nothing strips
it after placement — the runtime needs it anyway, so there is one mechanism
rather than two.

Each instance also has a **record** that is not scene data: source path, root
transform, overrides, removal list. That is what a save writes.

Undo mostly works already. `EditCommand` is a
`std::variant<ComponentDelta, EntityDelta>` built to be extended, and a
subtree delete is already several `EntityDelta`s in one `Transaction`. So
placing an instance is five `EntityDelta`s in one transaction, and pruning is
one `ComponentDelta`. The gap is the record, closed by a **third variant,
`InstanceDelta`** — so placing an instance is one `InstanceDelta` plus its
`EntityDelta`s, undone atomically.

The record stays beside the scene, as editor state. Putting it in a component
would ride existing machinery but would be scene data that must never ship,
and would quietly recreate a privileged root-ish member.

### What the editor shows

- **Outliner: one collapsible row per instance** — *car_3 (car.abp)* with
  members nested under it, and nested instances as nested rows. Clicking the
  row selects the instance; expanding selects members. Selection therefore has
  two modes.
- **Entity picker: grouped and labelled.** The `EntityRef` field editor
  previews entities as `Entity [41:0]` today, which is already hard to pick
  from and unusable once a level holds forty cars. It shows `car_3 › wheel_fl`
  instead, from the same tag. The eyedropper is unaffected.
- **Inspector: a header block** on a member — which blueprint, which member,
  which instance — plus **overridden fields visibly marked**, a **reset** on
  each (per field and per component), and a button selecting the whole
  instance.

That marking is also the cheap version of the validator this design skips
(see *Deliberately omitted*): a field shown as overridden that you never
touched is visible immediately.

### Editing a blueprint re-expands its instances

Editing `car.abp` with a level of forty cars open **re-expands all forty**,
re-applying each instance's overrides. "Edit the blueprint, see it everywhere"
is most of why prefabs are worth having, and an editor that waits for a reload
reads as broken.

**It is an in-place diff, not a destroy-and-recreate.** A member that still
exists keeps its entity: the file's component values are re-applied onto that
same handle, then the instance's overrides. Only members the edit *deleted*
are destroyed, and only members it *added* are created.

That is not an optimisation. Entity handles are `(slot, generation)`, and
`EditHistory` stores exact handles while `Scene::ReviveAt` is valid "only for
a currently-free slot under a strictly linear history" (`Scene.hpp:76-95`).
Destroying and recreating forty cars behind undo's back means a later Ctrl-Z
revives into a slot something else now occupies. Level entities holding an
`EntityRef` to a member — which the picker above actively encourages — would
dangle for the same reason. In-place keeps every handle valid, so the common
case (a field edit) touches neither undo nor references at all.

When a blueprint edit *does* delete a member, the entities really are
destroyed, and undo entries naming them can no longer replay. **The history is
truncated below the oldest transaction that references any of them** — newer
transactions stay, because they never touched those entities, and everything
kept is guaranteed replayable. Truncating from the bottom is the only safe
cut: transactions are atomic, so a single command cannot be removed from one,
and the stack is linear, so a transaction cannot be removed from the middle.
The history panel makes the loss visible rather than leaving entries that
silently do nothing.

Both halves are revisitable. Unity updates prefab instances in place and
keeps undo; Unreal reinstantiates on Blueprint compile and does not. Starting
where a field edit costs nothing is the cheap end of that range.

Transient state invalidated by re-application — physics bodies, resolved
asset pointers — is rebuilt by the same *function* `EditHistory`'s rebind hook
dispatches to, but re-expansion is not an `EditHistory` apply, so the
invocation is new work rather than reuse.

Ship builds never take this path.

## 11. Caching: the prepared form

A blueprint is parsed **once** and cached, keyed by virtual path. Spawning a
hundred bullets must not re-read and re-parse `bullet.abp` a hundred times.

What is cached is not the JSON but a **prepared form**: one `BinaryCodec`
block per member, plus a fixup table recording which fields are entity
references and which member name each targets. Spawning is then create N
entities, decode the blocks onto them, patch the refs.

It has to be a decode rather than a byte copy. Components are not
memcpy-safe: `MeshRenderer` holds a `std::vector<AssetId>`
(`Components.hpp:62`), whose bytes are a pointer, so copying them into a
hundred bullets gives a hundred components sharing one allocation and
ninety-nine dangling the moment the first is destroyed. Reflection also
offers no generic way to copy a live component — `ComponentMeta` has JSON
serialize, default construct and `getMutable`, and nothing that clones.

`BinaryCodec` already handles the whole field set correctly, is what
replication uses, and needs no new codegen. It walks fields rather than
copying bytes, which is slower than a memcpy and far faster than re-parsing
JSON — and re-parsing JSON is the cost this exists to remove. If a profile
ever asks for more, the next step is a generated per-type clone on
`ComponentMeta`, which is the same shape reflectgen already emits for
everything else.

The blocks hold asset *ids*, not loaded meshes, so the spawn path must run
the same asset-resolve step the level load already runs — otherwise a spawned
car arrives with unresolved meshes.

This wants its own small cache. `Render::AssetCache` is shaped for GPU
resources and a blueprint is not one.

Cleared on level unload, never evicted during a level. Loading
`car_with_antenna.abp` warms `car.abp`, since the closure is walked anyway. A
ship build never invalidates — blueprints are not modified mid-game. The
editor's re-expand path invalidates the prepared form for the edited file and
rebuilds it first.

## 12. Prerequisite: physics ignores `Parent`

Bodies are created at the local pose and world poses are written back into
the local field. Nothing had hit it because nothing was ever both parented
and physics-driven — and a car is exactly that, so this is **step zero**
rather than an unrelated bug.

The archived tag has a working fix, and its shape is forced rather than
stylistic: `Physics` links `Core + ECS + Jolt` and deliberately not
`Runtime` (which links `Render`, and would poison the headless server's
link), while `Parent` lives in `Runtime`. So Physics cannot look up a parent
itself, and `PhysicsWorld::ParentWorldFn` — a resolver the App injects — is
the only shape that does not invert the module graph. Worth lifting rather
than re-deriving.

---

## Still open

Raised by the review pass (`docs/blueprint-concept-review.md`). The four
format-visible items it found are now settled in the sections above — named
references and their scoping (§6), the member index in place of a stored name
(§2), transform spaces and the gizmo's frame mode (§3), and removals as paths
(§5). What remains is editor and build-order work, decidable while building:

- **Whether `.abp` files eventually fold into a binary format** with a hash
  check. §9's manifest makes every blueprint's content load-bearing across
  the wire, so this is now about speed and tamper-resistance rather than
  correctness.

### Settled while reviewing

- **Override recording is transacted with the edit.** An inspector edit on a
  member writes the component *and* the instance record, and one `InstanceDelta`
  carries the record's before/after so both revert together — otherwise undo
  leaves a fake override, the disease §5 exists to prevent. The per-field
  override is derived by diffing the gesture's own before/after, which is
  explicitly **not** the computed-override mistake: that one compared the live
  scene against the blueprint *across* edits, which is why editing a blueprint
  froze fake overrides into every instance. Reading what one gesture did is the
  definition of recorded.
- **`ASYSTEM` registration survives the linker**, and not by luck.
  `cmake/AssisiReflect.cmake:83-86` compiles generated sources into an OBJECT
  library precisely because "OBJECT libraries are always fully included in the
  final link — unlike static library members, which the linker drops when no
  symbol is referenced." `ACOMP` already rides it; `ASYSTEM` uses the same
  path.
- **The `InstanceView` build edge is an explicit opt-in list** the build
  reads, the same shape as reflectgen's source list. Editing a listed `.abp`
  regenerates its header and recompiles call sites; unlisted blueprints are
  invisible to the build.
- **A failed spawn is all-or-nothing.** Expansion builds into a staging list
  and commits only on success, so a missing nested file three members in
  leaves no partial instance. The prepared-form cache makes this nearly free,
  since failure is then only possible on first parse.
- **Build order.** Physics/`Parent` (§12) → named entities and references,
  format v2 (§6) → expansion (§1/§3/§4) and the tag (§2) → overrides (§5) →
  the four verbs (§7) → editor (§10) → prepared form (§11) and the network
  work (§9) → `ASYSTEM` (§8), at which point `systems` joins the format →
  `InstanceView` codegen. Two notes: the format ships *without* `systems`,
  because a level cannot resolve system names until §8's catalog exists and
  converting four level files twice is cheaper than blocking on codegen; and
  §10's editor work does not depend on §7's codegen, so it must not be
  scheduled behind it.

## Deliberately omitted

Not oversights. Each has a known shape if it is ever wanted.

- **Adopting loose entities into an instance.** Would break the subset
  invariant, and with it validation, `InstanceView` generation, and safe
  orphan handling. A group of arbitrary entities with no file behind it is a
  different feature with a different name.
- **Transferring a member between instances.** Follows from the above: a
  pruned entity is loose and cannot join another instance. Physically moving a
  wheel to another car still works — reparent it and rewire its gameplay refs —
  it simply is not a member of the new instance, so `DestroyInstance` will not
  take it. The narrowly legal version, if it is ever wanted, is re-tagging into
  an instance *of the same blueprint* whose corresponding member slot is
  vacant; that preserves the invariant.
- **Detaching a nested group as its own instance.** Flattening (§4) means
  there is no inner instance to detach. If wanted:
  `DetachNested(world, id, "car")` re-tags every member named `car/…` with a
  fresh id and rewrites the names to their leaves. It stays legal because it
  splits along a boundary the file already defines, and it needs no change to
  the runtime model — it writes two fields on a handful of entities.
- **The override validator.** A debug-only pass re-expanding each instance
  and comparing field by field against the live scene, to catch an edit path
  that changed a value without recording an override. Skipped because the risk
  is largely designed out: the editor's capture sits in front of the *generic
  reflected-field editor* (`EditorInspector.cpp:1285`), so every component and
  every field is captured automatically. Only bespoke controls need their own
  capture, and the four that exist all have it. Build it if one ever goes
  missing.
- **Runtime overrides on spawn** (§5) and **profiles** (§8).

Carried over from the first attempt, still true:

- Levels already saved with fabricated overrides read as genuine on load. A
  manual reset per instance clears them; nothing self-repairs.

## What to do differently

The last attempt changed its own design three times *while* being
implemented — placement as its own field, then a synthetic root, then the
root's linkage inverted — and each turn was built through rather than
re-planned. All three were the same question, re-litigated at the keyboard.

It is settled above (§3). Settle the rest on paper too.
