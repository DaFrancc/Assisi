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
instances a runtime identity, and parented members to a synthetic root. All
three are reversed here.

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
| `profile` | *n/a — profiles are gone (§6)* | |

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
      "transform": { ... }, "overrides": { ... } }
  ]
}
```

---

## 2. Nothing survives into the game

Once expanded, members are ordinary entities. No root, no marker component,
no lineage, nothing to query. **Every trace of the blueprint is gone by the
time the game is running.**

This is the property the whole design leans on, and it is not merely tidy:

- Undo, physics, migration and networking see normal components, so no other
  system grows a blueprint special case. Anything that would require
  blueprint-aware code elsewhere is the wrong answer.
- A server spawning a car gets plain entities, so replication needs no
  blueprint awareness at all — members replicate because they are ordinary
  entities, and a client never reads the `.abp`.

The editor needs instance identity while editing. How it holds that is
**open** — see §8.

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
component. Which has a consequence — see §7.

## 4. Nesting

A file may instance other files. A parking lot is six cars.

- **Cycles hard-fail at load.** `a` containing `b` containing `a` expands
  forever, and it is unrecoverable if missed.
- **The uniform-scale rule applies at every level**, not just the outermost.
- Expansion composes the whole chain — lot placement, car placement, member
  local — and discards every root.

## 5. Overrides

A level entry is a source, a placement, and a set of overrides. Nothing else
is authored per instance.

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

**At runtime there are no overrides.** A spawned blueprint's entities are
yours to change however you like — there is no file to write, so nothing is
recorded.

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

## 7. Systems

**Profiles are gone.** A file lists the individual systems it needs, by
name. Longer, and more straightforward — and the door back is open: a
profile could return later as "a name that expands to a list," which is
purely additive and composes with itemised lists in a way today's
function-based profiles do not.

Removing them also fixes a misplacement. The sandbox's profile contains
`world.physics.SetContactReporting(true)` — the *level* knowing what
`BounceSystem` needs. That belongs to the system, so that anyone who names it
gets it, and a level that forgot the line stops producing a system that runs
and silently does nothing.

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

Three rules come with it:

- **An unknown system name in a file is a hard error at load**, never a
  warning. That is what makes renaming a system break loudly instead of
  quietly dropping it from every level that named it.
- **Duplicate names, missing `after`/`before` targets, and ordering cycles
  are a whole-tree build error** — the same shape as reflectgen's
  `--check-handlers` pass, reusing that machinery rather than inventing a
  second one.
- **Render phase implies `RenderContext &`**, every other phase
  `SystemContext &`. Codegen enforces it, which is a correctness check the
  manual `Register`/`RegisterRender` split leaves to the caller today.

### No gate

`RequireAny`/`RequireAll` are **not** part of `ASYSTEM`, and the existing
`SystemRegistry::RequireAny` should probably go with them.

A gate is a skip test, not a query — it can only ever be a conservative
superset, exclusions can never contribute to one, and reflectgen cannot
verify it because it reads a declaration and not a function body. Too loose
costs one call; **too tight is a system that silently never runs**, which is
the exact failure this whole document opens with. What it buys is a
function call and an empty query, and a system that needs better than that
can bail early where it has real information — as `BounceSystem` already
does, on an empty contact log.

Recorded so it is a decision rather than an oversight: the case that would
bring a gate back is a system whose *emptiness check is itself expensive*.
None exists today.

### Installation

- **Install on demand.** Instantiating content installs what it names, at a
  safe point. Loading a level installs its list. Runtime spawning makes this
  mandatory rather than optional: gameplay can spawn a car into a level that
  never had one.
- **Reclaim on a slow sweep** — roughly every 30 s, uninstall what nothing
  needs. Uninstalling needs the same safe point installing does. *How the
  sweep decides a system is unused is open — see §8.*
- **The list is authored, never derived.** Tempting to infer it from the
  members' components; wrong, and not hypothetically — every blueprint with
  a rigid body would declare a dependency on physics when almost none mean
  it. The editor may *suggest*; the author decides, and nothing is enforced
  afterwards.

## 8. Prerequisite: physics ignores `Parent`

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

Settled later, noted here so they are decisions rather than discoveries.

- **How the editor holds instance identity.** It needs to know that an entity
  is `wheel_fl` of instance 3 — to select the instance, move it together, and
  record an override. Either a side map keyed by entity, or a transient
  component that never reaches a file and is never added by the game's
  loader. The map is more paths to forget (undo, redo, play/stop restore all
  recreate entities); the component rides along with undo for free. *This is
  the only open item that does not touch the file format, so it can wait
  without blocking anything.*
- **How the reclaim sweep identifies an unused system.** Dropping the gate
  removed the thing it would have queried, and deriving from component
  presence is the same mistake the authored-list rule exists to prevent — it
  would silently uninstall every system that reads no components at all.
  Likely answer is an authored keep-alive signature on the catalog entry, but
  it is not decided.
- **The spawn API's return.** A caller needs the body to drive the car, and
  positional indexing is out for the same reason overrides key by name. Some
  value with a name lookup — held or discarded by the caller, never stored.
- **Whether `.abp` files are hashed into `LevelIdentity`.** A level now
  depends on the transitive closure of the content it instances, and the
  handshake covers the `.alvl` bytes only, so two machines with different
  `car.abp` build different static halves from an identical level hash.
  Deferred deliberately: shipped builds ship one asset set, and the intended
  fix is a binary format with a hash check. When that lands, hash the
  *content* and not the bytes — the level hash already had to learn this
  (`562aa5d`), because git checks text out as CRLF on Windows and two
  machines disagreed about identical files.
- **Collapsing profiles into the catalog.** Not doing it now, but the catalog
  should be designed so a "profile" could later be a named set of catalog
  entries rather than a function — otherwise the two mechanisms become
  permanent by accident.

Carried over from the first attempt, still true:

- Levels already saved with fabricated overrides read as genuine on load. A
  manual reset per instance clears them; nothing self-repairs.
- Nothing validates that recorded overrides still match reality. An edit path
  that bypasses capture would go unnoticed, and a debug-only checker is the
  net for it.
- **"Explode an instance"** — break the link, keep the entities — is unbuilt
  and should be all-or-nothing.

## What to do differently

The last attempt changed its own design three times *while* being
implemented — placement as its own field, then a synthetic root, then the
root's linkage inverted — and each turn was built through rather than
re-planned. All three were the same question, re-litigated at the keyboard.

It is settled above (§3). Settle the rest on paper too.
