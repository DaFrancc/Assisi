# Review: blueprint-system-concept.md

Adversarial pass against the doc and the codebase it cites. Every code claim
checked is listed under Verified/Wrong; findings cite where they were checked.

## Verdict

The core shape is sound and most codebase citations check out: one format, the
evaporating root, flattening, the subset invariant, and §12-as-step-zero are
all well grounded and internally consistent with each other. But the brief is
**not yet settled enough to start**: the override merge across nesting is
undefined exactly where it will be hit first (`null` under an outer field
override), §10's re-expansion silently destroys the entity-identity guarantees
the undo system is built on, §7's `FindInstance<Car>` requires runtime state
§2 says does not exist, and §11's "copy blobs in" cannot be implemented
against the reflection system as it stands. All four are paper-fixable in an
afternoon; all four would otherwise be re-decided at the keyboard, which is
the failure mode the doc's own closing section warns about.

---

## Blocking

### B1. The override merge is undefined for `null` across nesting levels

§5 defines "outermost wins, per field" for *fields* and defines `null` as
component removal — but never defines their interaction. Concretely:
`car.abp` declares `RigidBody`; `lot.abp` overrides `car_3: { RigidBody: null }`;
a level overrides `lot_a/car_3: { RigidBody: { mass: 5 } }`. Three readings
are all defensible:

1. Removal is inner, field override is outer, outermost wins → component
   resurrects with `mass: 5` and every other field from `car.abp`. Surprising:
   an inner author deleted it and an outer field edit brought the whole thing
   back.
2. Removal wins; the outer field override is dropped with a log line
   (symmetric with §6's orphaned-override handling).
3. `null` and `{...}` are both *component-level* claims; per-field merge only
   applies between two object claims; outermost component-level claim wins →
   the level's `{ mass: 5 }` reads as re-add, same as reading 1 but by a
   different rule that gives a different answer when the outer entry is empty
   `{}`.

Also undefined: two levels both *adding* the same component (`Turret` added by
`lot.abp` and by the level, different fields) — per-field merge of two adds is
coherent, but whose defaults fill the unspecified fields must be stated (the
component's C++ defaults, presumably — say so).

**Why it matters:** this is the single most-exercised code path in override
application, and the doc's precedent (the §5 bullet arguing against
whole-set replacement "is how someone loses edits") pulls toward reading 1
while §6's orphan logic pulls toward reading 2.

**Resolution:** define the merge as a two-tier lattice on paper: claims are
per-component (`null` | object), ordered inner→outer; an outer *object* claim
merges per-field into the inner result **only if** the inner result still has
the component; an outer field override on an inner-removed component is
dropped with a log (reading 2). Re-adding after an inner removal requires the
outer level to state the component as an add — and say explicitly that an add
starts from the component's C++ defaults, not from the blueprint's removed
values. Whatever is chosen, write the three-level example above into §5.

### B2. §10 re-expansion breaks entity identity, undo, and external refs — and the doc claims "undo mostly works already"

Re-expanding forty cars means destroying and recreating hundreds of member
entities *outside* the undo system. Three things break, none mentioned:

- **Undo history dangles.** `EditHistory` stores exact `(index, generation)`
  handles and its apply relies on `Scene::ReviveAt`, which is documented as
  valid "only for a currently-free slot under a strictly linear history"
  (Scene.hpp:79–95; EditHistory.hpp:51, 78). Re-expansion is a mass
  destroy/create that history never saw; a later Undo will `ReviveAt` into
  slots the re-expansion re-occupied — that is an assert at best, silent
  corruption at worst.
- **External `EntityRef`s dangle.** §10's entity picker deliberately makes it
  easy for a loose level entity to reference `car_3 › wheel_fl`. Recreated
  members get fresh handles; every such ref goes stale on every blueprint
  edit.
- **Selection and gestures** referencing members die mid-frame.

Also, the claim that invalidated transients are "rebuilt through the rebind
hook `EditHistory` already uses" overstates the reuse: the hook
(EditHistory.hpp:109–116) fires from EditHistory's own apply path. Re-expansion
is not an EditHistory apply, so the *hook function* can be shared but the
invocation machinery is new work.

**Resolution:** specify re-expansion as an **in-place diff, not
destroy-and-recreate**: members that survive the edit keep their entity
handles (components re-applied onto the same entity; transforms recomposed),
members the edit deleted are destroyed, members the edit added are created.
That preserves undo handles, external refs, and selection in the common case
(field edits). For deleted members, state that history referencing them is
handled the same way `EndFrameSweep` abandons gestures for dead entities — or
simply that blueprint edits clear the open level's history (cheap and
defensible; pick one on paper).

### B3. `FindInstance<Car>` and spawn-time validation need runtime state §2 says does not exist

§2: "Exactly one thing [survives]: a tag… There is no root entity, no marker
beyond this tag, no lineage tree, and no stored member list." But:

- `FindInstance<Car>(world, id)` (§7) must verify that instance `id` was
  spawned from `car.abp` — otherwise it happily builds a `Car` view over a
  crate's members. Nothing in the surviving state records which source an id
  came from.
- "Validating an instance against its source" (§5's stated payoff of the
  subset invariant) needs the same id→source mapping at runtime.
- `SpawnBlueprint` needs an id allocator; **nothing specifies where instance
  ids live, whether they are per-world or global, whether they reset on level
  load, and what happens on wrap** — and the editor's records (§10) key on
  them across save/load, so this is observable, not internal.

**Resolution:** admit a small runtime instance table — id → source path (and
probably the root transform, which the editor's record needs anyway) — as the
*second* surviving thing, owned by the world, never serialized into the scene,
never replicated. Then restate §2 as "nothing survives **on the entities**
except the tag", which is the invariant the four concrete failures actually
depend on. Decide id scope (per-world, reset on level load, 0 reserved) in
the same paragraph.

### B4. §11's prepared form ("copy blobs in") is not implementable against the current reflection system

Components are not memcpy-safe: `MeshRenderer` carries
`std::vector<Assisi::Core::AssetId> materialOverrides`
(modules/Runtime/include/Assisi/Runtime/Components.hpp:62), and
`AssetIdVector`/`AssetPathVector` are first-class field types
(FieldMeta.hpp:33–35). A raw binary-layout blob of such a component copies
heap pointers. And the reflection surface offers **no clone hook**:
`ComponentMeta` has JSON `serialize`/`addToScene`, default `construct`, and
`getMutable` (ComponentMeta.hpp:33–85) — nothing that copies a live component
generically.

Three real options, each with different codegen cost:

1. **Generated per-type clone** (`void clone(const void* src, void* dst)` on
   `ComponentMeta`) + a prototype store — closest to the doc's "copy blobs
   in", needs a reflectgen extension.
2. **BinaryCodec blocks** decoded per spawn — the codec already exists,
   handles the whole field set safely, and "full state is a delta against the
   empty baseline" (BinaryCodec.hpp) is exactly the shape needed; cost is a
   per-field decode per spawn rather than a copy, still far cheaper than JSON.
3. **Prototype scene** + typed copy via a generated visitor.

**Resolution:** pick one in §11 — option 1 or 2; and note that the fixup
table's "which fields are entity references" is already answerable from
`FieldType::EntityRef`, which *is* supported (FieldMeta.hpp:30). Also state
that the runtime spawn path must run the same asset-resolve step the load
path runs (the blobs hold `AssetId`s; the transient `meshBuffer`/`materials`
pointers must be resolved post-spawn, as `ReresolveEntityAssets` does in the
editor).

---

## Should fix

### S1. §2's own example violates §2's own principle

"Calling `DestroyInstance(member.instanceId)` from a death handler is
correct." A death handler that reads `BlueprintMember` **is** a system
branching on membership: a car assembled in code has no tag, so the handler
destroys the group on a placed car and silently does nothing on a hand-built
one — which is verbatim failure #1 in §2's own list. The
argument-vs-query distinction does not rescue it; to hand the id back you
must read the tag, and presence/absence of the tag changes behaviour.

**Resolution:** the death-handler case belongs to §2's own alternative —
gameplay wiring (a `VehicleParts`/refs component authored in the file, which
works identically on hand-assembled cars). Restrict the tag-as-argument rule
to tooling and editor call sites, and change the example, because it is the
sentence every implementer will quote back when adding gameplay reads of the
tag.

### S2. `memberName` as `ShortString` breaks under path composition — and it is a design issue, not the "implementation detail" Still-open calls it

`ShortString` is `TrivialString<32>` (ShortString.hpp:15). Path-composed names
— `parking_lot_a/car_3/wheel_fl` is 26 characters — overflow at two levels of
nesting with ordinary names. Truncation makes two members collide, which
silently retargets overrides and `FindMember` — the exact class of bug §6's
naming rule exists to kill. Since overflow behaviour changes which member an
override addresses, this is format-visible.

**Resolution:** decide on paper: either the tag stores a hash + the runtime
instance table (B3) owns the full path strings, or the field is a heap string
in the (unserialized, unreplicated) tag, or nesting depth × name length is
capped with a hard load error. Any is fine; unstated, it will be re-decided.

### S3. Named `EntityRef`s are a file-format migration the doc implies but never states — and cross-boundary refs have no syntax at all

§6 replaces positional identity with names, and §2 says member refs are
rewritten at expansion "the same rewrite `Parent` gets". Today `EntityRef`
serializes as a positional serial index (FieldMeta.hpp:30; SceneSerializer.hpp
:181–184, "level files on disk use remapped serial indices"), and the
`EntityToIndex`/`IndexToEntity` context is index-based and explicitly
non-reentrant per thread (SceneSerializer.hpp:186–188). So this needs:

- a new named mapping mode (a fourth, after remap/raw/transfer — the doc's
  own file list calls out that three already exist);
- a stated migration story for version-1 levels (auto-upgrade on load? a
  converter? refuse?) — never mentioned;
- **a file syntax for refs that cross the instance boundary**: a loose level
  entity whose `Wheel.chassis` points at `car_3/body` (the §10 picker
  actively encourages this), and an override whose `EntityRef` field points
  at a loose level entity. Neither direction has a specified representation.

**Resolution:** specify `EntityRef` file values as names/paths, define the
two cross-boundary forms (e.g. `"car_3/body"` from level scope; and either
forbid override refs to level entities or scope them), and state the v1
migration.

### S4. Override recording and undo must be transacted together on *every* member edit — `InstanceDelta` as described only covers placement

§5: "an override exists because somebody edited that field." So every
inspector edit on a member writes two things: the component (captured by
`EditHistory::RecordBefore`, EditorInspector.cpp:1284–1287) and the instance
record. Undo of that edit must revert both, atomically — otherwise undo
restores the value but leaves a fake override claim, which is the
fabricated-override disease §5 was redesigned to kill. §10 presents
`InstanceDelta` only for placing/pruning.

Also a granularity mismatch worth blessing explicitly: overrides are
per-field, but capture is per-component whole-JSON before/after
(EditHistory.hpp:49–55). Deriving the per-field override from a gesture means
diffing the gesture's own before/after. That is *not* the computed-override
mistake (which compared live scene against the blueprint across edits) — but
someone holding the "recorded, not computed" rule will refuse to write the
diff unless the doc says a per-gesture diff is the recording.

**Resolution:** extend §10: every member edit produces a transaction pairing
the `ComponentDelta` with the record mutation (an `InstanceDelta` carrying
the record's before/after, or a fourth variant `OverrideDelta`); and add one
sentence to §5 blessing per-gesture diffing.

### S5. Member transforms: which space is authored, stored, and overridden is never stated

The file stores members' root-relative locals; the scene after expansion
stores composed transforms; the root "does not exist after expansion" but
*does* exist as an editor entity (§3). Unanswered questions, each a
keyboard-decision waiting to happen:

- When the user drags a member's gizmo, the override recorded in the file
  must be in **file space** — which requires inverse-composing the instance
  (and nested) root chain out of the scene-space edit. Stated nowhere.
- Moving the *root* updates the record's transform and rewrites every member
  transform — and must **not** record per-member overrides. Stated nowhere.
- After load in a ship build there is no root; if the editor keeps one, the
  uniform-scale constraint is checked where, and what happens on violation
  (clamp? refuse? log)?

**Resolution:** one paragraph in §3 or §5: member `Transform` overrides are
recorded in file-local space by inverse-composing the root chain; root motion
touches only the record; non-uniform root scale is refused at the gizmo/
inspector (editor) and hard-fails at load (files hand-edited around it).

### S6. There is no build order, and one dependency is real: §8 gates the file format

An order that works: **§12 physics/Parent → §6 named entities (+ format-v2
groundwork, S3) → expansion core (§1/§3/§4) → §5 overrides → §7 tag + four
verbs → §9 closure hash → §10 editor → §11 prepared form → §8 ASYSTEM →
InstanceView codegen.** Two things worth writing down:

- The proposed file format's `systems` array (§1) **depends on the §8
  catalog existing** — a level cannot name systems until something resolves
  names. Either ASYSTEM lands before the format switch, or format v2 keeps
  `profile` transitionally and `systems` arrives with §8. The doc presents
  them as one simultaneous v2.
- §10 does *not* depend on §7's codegen (the editor uses only the tag and the
  record; `InstanceView` is opt-in) — worth stating so nobody serializes the
  editor behind reflectgen work.

### S7. "The same safe point that already applies `RequestTravel` and `FlushDestroyed`" — those are two different points

Deferred loads and travel land at the `DrainMain` safe point, *before*
`OnUpdate`'s systems run (Application.cpp:510–518); `FlushDestroyed` runs at
end-of-frame per host (Application.hpp:122; call sites in EditorApp.cpp:990,
ServerApp.cpp:215/329, NetSession.cpp:121 — there is no single engine-owned
call). §8's deferral argument is right; the sentence just names a single
point that does not exist, and the implementer will have to pick. The
`DrainMain` point is the correct one for installs (no phase mid-walk, before
the frame's systems), so name it.

---

## Worth considering

### W1. The founding failure recurs over the network, and only its hash-half is recorded

A blueprint spawned in C++ on the host replicates its members to clients as
ordinary entities — correct per §2. But the client never ran
`SpawnBlueprint`, so it never installed the systems the blueprint names. A
client-side system the content needs (visual effects, prediction, audio)
silently does not run — the exact "component whose system was never installed
just does nothing" the document opens with, now cross-machine. §9 records the
*hash* hole for code-spawned blueprints; the *behaviour* hole is unrecorded.
Record it the same way (likely answer: the level's authored list must cover
anything spawnable at runtime, or the future binary format carries a manifest
clients pre-install).

### W2. "Linking a module registers its systems" meets the static-library dead-strip

ASYSTEM registration will live in generated TUs whose only content is static
initializers. Linkers do not pull archive members no symbol references — a
game module containing *only* systems (perfectly plausible under §8's model,
since systems keep no state) could silently register nothing, which is again
the silent-do-nothing failure. ACOMP registration works today, so a mechanism
exists (object libraries, whole-archive, or incidental symbol references) —
verify which, and state that ASYSTEM rides the same one, because "it worked
for components" may be an accident of every module currently exporting
referenced symbols.

### W3. `InstanceView` codegen creates a content→build dependency — say who owns it

The generator reads `.abp` files at build time, so editing stable content
recompiles call sites. That is the accepted price of the feature, but the
build-graph edge (which files are inputs, how the build knows a blueprint is
"opted in", what happens when the file and the generated header drift between
runs of the generator) is unstated. One sentence — e.g. an explicit opt-in
list the build reads, same as reflectgen's source list — prevents this being
designed ad hoc.

### W4. Spawn failure is half-specified

`instanceId == 0` marks a failed spawn (§7) — but not what the scene looks
like afterwards when a *nested* file is missing or corrupt three members into
expansion. Recommend all-or-nothing per spawn: expand into a staging list,
commit only on success (the prepared-form cache in §11 makes this nearly
free, since failure is then only possible on first parse).

---

## Verified claims

All checked against the working tree; the doc can cite these with confidence.

- **Level format is `version`/`profile`/`entities`** — SceneSerializer.hpp:11–25, 55–67.
- **`Scene::Destroy` is deferred to `FlushDestroyed`; calling from inside a
  system is safe** — Scene.hpp:48–74; Query docs at 293–296 confirm mid-loop
  Destroy safety.
- **`ReviveAt` exact-identity semantics** (and its strictly-linear-history
  constraint, which B2 turns on) — Scene.hpp:76–95.
- **`GatherSubtree` is opt-in hierarchy destruction at the call site** —
  Hierarchy.hpp:62–68.
- **`Parent` link is positional in files**: `EntityRef` serializes as a
  remapped serial index — FieldMeta.hpp:30, SceneSerializer.hpp:181–184; so
  §6's hand-edit retarget hazard is real.
- **`BlueprintMember` "never replicated" is automatic** — replication is
  opt-in per type via `ACOMP(replicable)`
  (ComponentRegistry.hpp:70–92 replicable ordinals; the inspector even
  labels non-replicable types, EditorInspector.cpp:1291–1299). A plain
  `ACOMP()` needs no explicit exclusion. Verified as claimed.
- **`SystemRegistry` invalidates its cached order on registration**
  (SystemRegistry.hpp:135–137, per-phase `dirty`), and **`RequireAny` is a
  cheap per-frame skip** (SystemRegistry.hpp:165–177;
  Scene::ComponentCount is "two loads and a compare", Scene.hpp:219–232).
- **The sandbox profile contains `world.physics.SetContactReporting(true)`**
  — apps/sandbox/src/main.cpp:274. `registerGameSystems` exists to be
  deleted (sandbox main.cpp, EditorApp.cpp).
- **`--check-handlers` exists in reflectgen** — tools/reflectgen/reflectgen.py:303.
- **`MessageTraits<T>` is an undefined primary template specialized by
  generated code** — MessageMeta.hpp:119; §7's "second instance of an
  existing pattern" claim holds.
- **`EditCommand` is `std::variant<ComponentDelta, EntityDelta>` with
  transactions and a rebind hook** — EditHistory.hpp:85, 89–96, 109–120.
- **Editor capture sits in front of the generic reflected-field editor at
  EditorInspector.cpp:1285** — verified; `RecordBefore` at 1284–1287,
  immediately before `EditComponentFields`.
- **`EntityRef` picker previews as `Entity [41:0]`** — EditorInspector.cpp:420–422.
- **`LevelIdentity.contentHash` hashes file content; mismatch refuses the
  join** — NetProtocol.hpp:188–196. **Commit `562aa5d` is the CRLF fix** —
  "fix(net): a level's content hash stops caring about line endings".
- **§12 physics claims, all of them**: bodies are created from the local
  `Transform` (PhysicsWorld.cpp:526, and the descriptor query at 552);
  world poses are written back into `Transform` (InterpolateTransforms,
  PhysicsWorld.cpp:685+); Physics links exactly `Core + ECS + Jolt`
  (modules/Physics/CMakeLists.txt:19–21); the archived `ParentWorldFn` fix
  exists on the tag
  (`archive/blueprints-attempt-1:modules/Physics/.../PhysicsWorld.hpp:113`).
  Lifting it rather than re-deriving is the right call.

## Wrong claims

- **"the same safe point that already applies `RequestTravel` and
  `FlushDestroyed`"** (§8) — no such single point exists; travel/deferred
  loads land at `DrainMain` before systems, `FlushDestroyed` at end of frame
  per host. See S7 for cites and the fix.
- **"Exactly one thing [survives] into the game"** (§2) — not sustainable as
  written; `SpawnBlueprint`'s id allocator and `FindInstance<Car>`'s
  id→source check both require world-owned instance state. See B3. (The
  *spirit* — nothing extra on the entities, nothing serialized, nothing
  replicated — survives intact.)
- **"transient state … is rebuilt through the rebind hook `EditHistory`
  already uses"** (§10) — the hook *function* is reusable; the invocation
  machinery is EditHistory's apply path, which re-expansion does not go
  through. Minor, but it hides real work. See B2.

Nothing else checked was wrong. Line-number-level citations in the doc
(EditorInspector.cpp:1285, `562aa5d`) were exact.

## Keep

These are genuinely well decided; do not reopen them.

- **One format, one loader, two verbs** — including refusing to gate on
  extension. The strongest decision in the doc, and the Godot/Unreal evidence
  is fairly deployed.
- **The evaporating root, flattening, and the uniform-scale constraint** —
  the shear argument makes the constraint load-bearing rather than taste, and
  flattening's payoff (one id, no instance tree, no "which level did you
  mean" API) is real. §4's rejected alternative is described accurately
  enough to stay rejected.
- **Recorded-not-computed overrides**, with the concrete failure of the
  computed version documented. Keep the fabrication warning carried over from
  attempt 1.
- **The subset invariant and everything hanging off it** — no adopt, no
  member addition, prune/explode only — with three named payoffs. The
  "Deliberately omitted" entries each record the shape a future feature would
  take, which is exactly how to keep them from being improvised later.
- **No renaming, deletion drops with a log** — the deleted-or-renamed
  ambiguity argument is airtight.
- **Membership-never-load-bearing as a principle** and its four concrete
  failures (fix the one bad example, S1 — the principle itself is right).
- **No runtime overrides; spawn-then-write** — typed, and it kills a whole
  API surface.
- **`InstanceView`'s receipt rules** — move-only, fields-only enforced by
  regeneration clobbering, reflectgen ban on views inside components. Closing
  it "by machinery, not discipline" is the correct instinct, and the naming
  argument (`View`, not `Spawned`) is sound.
- **No gate on ASYSTEM, with the reversal condition recorded** — an
  expensive-emptiness-check system — so it is a decision, not an oversight.
- **Deferred install / never uninstall / authored-never-derived**, each with
  its reason attached (the derive-from-components counterexample is
  specific and correct).
- **Hash the content, not the bytes**, with the CRLF receipt; and recording
  the code-spawn hole in §9 instead of pretending it is closed.
- **§12 as step zero, lifting the archived fix** — verified feasible above.
- **The closing section itself.** "Settle the rest on paper too" is the right
  instruction; the Blocking items above are the list of what is not yet on
  paper.
