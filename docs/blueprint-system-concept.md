# Blueprints — the concept

What the system is for and what it has to do, with none of the mechanism.
This is the brief for building it.

There was a first attempt — the whole thing, built in a day and then set
aside. It is not in this tree. It lives on the tag
**`archive/blueprints-attempt-1`**, along with its 1174-line design record
(`docs/blueprint-design-notes.md`), which has the file format, the review
dispositions and the engine surveys if any of that is ever wanted:

    git show archive/blueprints-attempt-1:docs/blueprint-design-notes.md

Everything below is what that attempt was worth keeping.

## Why

Levels today author every component inline. The tenth bouncy cube copies
nine components again, and fixing its restitution edits ten levels. Worse,
the ECS split between data and code fails silently: a component whose system
was never installed just does nothing, with no error and nothing to
diagnose.

So: **most level content should be blueprint instances.** A level lists, per
instance, which blueprint and what that instance changed — nothing else.
Levels stay small, a blueprint fix propagates on next load, and a
blueprint's behavior comes with it instead of depending on the level's
profile happening to install the right systems.

## What a blueprint is

An asset file holding **several entities**, not one. A car is a body plus
four wheels; that is the median case, not an edge case. Members are named,
those names are stable and author-visible, and references between members go
by name. Alongside the entities, the blueprint names the **systems its
behavior needs**.

## What a level entry is

A blueprint path, a placement, and a set of overrides. Nothing else is
authored per instance.

## The eight things that make it work

1. **Expand on load.** Once instantiated, members are ordinary entities that
   happen to know their lineage. Undo, physics, migration, networking see
   normal components — no other system grows a blueprint special case. The
   whole design leans on this; anything that would require blueprint-aware
   code elsewhere is the wrong answer.

2. **Instances are first-class.** Every member knows its blueprint, its
   instance, and which member it is. Everything else — grouping, "select the
   whole thing", "how many of these are in the level", reclaiming systems —
   is *derived* from that, never counted or cached.

3. **Members are keyed by name, never by position.** Adding a member to
   `car.abp` must not re-target the overrides in shipped levels. Every engine
   that tried positional or name-fragile keying broke on insert/reorder and
   retrofitted stable ids.

4. **Overrides are recorded, not computed.** An override exists because
   somebody edited that field, not because a comparison found a difference.
   The computed version was built and it was wrong: editing a blueprint while
   a level is open silently froze the old values into every instance as fake
   overrides. Whatever the storage looks like, this is the property to keep.

5. **Un-overridden means un-resolved.** A field nobody touched re-reads from
   the blueprint on every load. That is what makes "fix it once, fixed
   everywhere" true, and it is the whole point of the format.

6. **Reset is a first-class gesture, at every scope.** Per field, per
   component, per member, whole instance — each drops the claim and lets the
   value fall back. Every reset goes through undo as one entry per gesture,
   not one per component it happened to touch.

7. **Systems install on demand and are reclaimed.** Instantiating a blueprint
   installs what it names, at a safe point. A periodic sweep uninstalls what
   nothing needs any more. Installation is always someone *asking* — a load,
   a spawn, an undo that brings an entity back — so only the reclaim half
   needs to be a sweep.

8. **The system list is authored, never derived.** Tempting to infer it from
   the members' components; wrong, and not hypothetically — every blueprint
   with a rigid body would declare a dependency on physics when almost none
   mean it. A component being present is not the same as the blueprint's
   contract depending on the system that reads it. The editor may *suggest*,
   pre-ticking only where a component is some system's exclusive signal; the
   author decides, and nothing is enforced afterwards.

## The editor half

Authoring is the half that makes the rest usable, and a plan that only
instantiates blueprints leaves hand-written JSON as the only way to make one.

- **Create from selection** — take what's selected, write a blueprint,
  convert the selection into an instance of it, one undoable step.
- **Spawn** — place an instance from the blueprint list.
- **Override display** — overridden fields are visibly marked, with the reset
  gestures above.
- **Relationships** — selecting a member selects/outlines the instance; the
  entity list groups members under the blueprint's name; the inspector can
  jump between members and find the other instances. Click selects the
  instance, double-click drills into a member.

## The instance root

An instance needs one thing you can grab to move it. The last attempt made
that a separate invisible entity, and that part was right: it means no member
is special, and the "except the root" caveat stops being repeated everywhere.
It gets the same billboard icon any mesh-less entity gets, and it only ever
translates, rotates, or scales uniformly.

What was **wrong** in that attempt: it parented the members to it. The root
is an **editor-only linkage** — grouping and a drag handle. Members should
carry no parent component because of it.

Which leaves the question that has to be answered before any of this is
written again:

> When the editor moves an instance, what lands in the file?

Either the members' transforms are stored as-moved (world space, nothing
composes or decomposes), or they stay relative to the root (load composes,
save decomposes). The second was rejected once already, for good reasons —
matrices compose one-way and decomposing stored data is lossy, and a
mid-session blueprint edit would make the placement silently absorb the
inverse of the edit. Decide this first; it determines the entire save path.

## Known open ends

- Physics ignores parenting in both directions — bodies are created at the
  local pose and world poses are written back into the local field. Nothing
  had hit it because nothing was ever both parented and physics-driven. It is
  a real bug independent of blueprints.
- Levels already saved with fabricated overrides read as genuine on load. A
  manual reset per instance clears them; nothing self-repairs.
- Nothing validates that the recorded overrides still match reality. An edit
  path that bypasses capture would go unnoticed, and a debug-only checker is
  the net for it.
- "Explode an instance" — break the link, keep the entities — is unbuilt and
  should be all-or-nothing.

## What to do differently

The last attempt changed its own design three times *while* being
implemented, and each turn was built through rather than re-planned. The
decisions above are the ones that were expensive to reach; settle the open
ones on paper before writing code.
