# Editor undo/redo — design notes (planning)

Status: **design settled, not started.** This is the authoritative plan for the
editor's scene undo/redo system. Reviewed by an independent pass (Fable 5) that
found several verified flaws in the first draft; those fixes are folded in below
and marked where they change the naive approach.

## 1. Goal & scope

- **Ctrl-Z undo / Ctrl-Y + Ctrl-Shift-Z redo** in the sandbox editor.
- Track **all edits to a scene**: component field edits, the transform gizmo,
  add/remove component, rename, asset assignment (mesh/material/EntityRef), and
  entity create/delete.
- **Editor-only.** None of this logic may exist in a shipped game (see §9).
- Not in scope: undo *during* Play mode (physics owns Transforms then); undo of
  asset-level operations (`.amat` regenerate, reimport) — those are asset-db
  concerns, not scene edits.

## 2. Prior art (what other engines do)

- **Unreal** — a transaction buffer. Before mutating a `UObject` you call
  `Modify()`, which serializes that object's *reflected properties* into the open
  transaction; undo restores the serialized state. Grouped with
  `ScopedTransaction`. Editor-only (`UnrealEd`). Reference-safe because UObjects
  have stable identity (pointers/names).
- **Unity** — `Undo.RecordObject(obj, "label")` snapshots an object's serialized
  state *before* a change, **plus explicit** `RegisterCreatedObjectUndo`,
  `DestroyObjectImmediate`, `AddComponent` for lifetime ops. Stable instance IDs.
- **Godot** — `EditorUndoRedoManager`: classic command pattern, you register
  do/undo method+property calls explicitly (`add_do_property`/`add_undo_property`,
  `add_do_reference`).
- **Blender** — global memfile (serialized-whole-file) snapshot undo, mitigated by
  per-mode local undo. The memory-heavy outlier the others avoid.

**Takeaway:** the mainstream (Unreal/Unity) is **reflection-serialized per-object
deltas inside transactions** — precisely the model chosen here — *not* bespoke
command objects per field, and *not* whole-scene snapshots. None of them hit our
generational-handle problem because their identity is stable; our `Scene::ReviveAt`
(§7) is how we buy that same stable identity.

## 3. Model decision

**Command-based, stored as a `std::vector` of tagged unions (`std::variant`),
payloads captured via the reflection layer as JSON.** Rejected alternatives:

- **Whole-scene snapshot per edit** (Blender) — cost scales with scene size, not
  edit size; still needs the transient-state rebuild anyway. Rejected.
- **Polymorphic command objects** (`vector<unique_ptr<ICommand>>`, virtuals) —
  per-edit heap allocation + vtable indirection, and clashes with the codebase's
  value-type style. Rejected in favor of the variant.
- **Byte-copy of components** — components are **not** trivially copyable
  (`MeshRenderer` holds `std::vector`s + transient pointers; `Name` holds a
  ShortString), and transient fields must be excluded — which reflectgen already
  does. Rejected; reflection-JSON is the right payload.
- **Change-tick auto-capture** (drive capture off `ACOMP(tracked)` change
  detection) — **does not work**: (a) only `Transform` is `ACOMP(tracked)`, so
  `MarkChanged` is a no-op for every other pool (`Scene.hpp` stamps only when the
  pool has a non-null change lane); (b) a tick tells you *that* something changed,
  never *what it was before* — you can't reconstruct `before` post-hoc without a
  prior snapshot. Use ticks only as a **debug tripwire** (assert no tracked
  component changed outside an open transaction).

## 4. Data structures

```cpp
// One reversible edit. optional = "component/entity absent", so this one shape
// covers value-change AND add/remove (absent <-> present).
struct ComponentDelta {
    ECS::Entity           entity;   // exact (index, generation) while alive
    Reflect::ComponentId  id;
    std::optional<nlohmann::json> before;   // nullopt = component absent
    std::optional<nlohmann::json> after;
};

// Whole-entity lifetime edit (create/destroy), possibly a subtree.
struct EntityDelta {
    ECS::Entity handle;                     // exact (index, generation) while alive
    std::optional<std::vector<ComponentDelta>> before;  // nullopt = entity didn't exist
    std::optional<std::vector<ComponentDelta>> after;
    // Stored as per-component blobs (not one opaque JSON) so two-phase apply
    // (revive entity, then add components) is natural — mirrors SceneSerializer
    // Load pass 1/2.
};

using EditCommand = std::variant<ComponentDelta, EntityDelta>;

struct Transaction {
    std::string              label;         // "Move", "Add MeshRenderer", ...
    std::vector<EditCommand> cmds;
    ECS::Entity              selectionBefore = ECS::NullEntity;
    ECS::Entity              selectionAfter  = ECS::NullEntity;
};

class EditHistory {
    std::vector<Transaction> _undo, _redo;
    bool                     _applying = false;  // re-entrancy guard (§8.11)
    // + open-gesture scratch state for the record-before-write capture (§5)
};
```

- `EditCommand` variant is the right container. `EntityDelta` uses a `vector` of
  per-component blobs so multi-entity/subtree deltas apply two-phase.
- History is `vector<Transaction>`, **not** raw commands — a transaction is one
  user gesture (a gizmo drag, a slider drag, one add-component), so it's one Ctrl-Z.
- **Depth cap** (e.g. 128–256 transactions, drop oldest). nlohmann objects are
  heavy; consider storing `dump()`ed strings. Cap regardless.
- **Never persist to disk**: `ComponentId` is a per-run alphabetical dense id
  assigned at registry finalize; it is not stable across builds.

## 5. Capture architecture — record-before-write (the key design choice)

Do **not** hand-write rising/falling-edge detection at every edit site — that is
exactly where "track ALL edits" leaks. Adopt **Unity's `RecordObject` model**:
the only per-site obligation is *declare intent before writing*.

Why not post-hoc: ImGui writes the value *inside* the widget call (e.g.
`DragFloat`), so by the time you can test `IsItemActivated()`/`edited` the
component memory is already modified — "capture before at gesture start" would
capture an already-changed value. So capture must be **pre-write**.

Mechanism:

1. **`history.Record(entity, componentId, label)`** — captures `before` (serialize
   the component now, or nullopt if absent) into an open-gesture scratch keyed by
   `(entity, componentId)`. Idempotent: a second `Record` for an already-open key
   reuses the existing `before`.
2. **Inspector**: one `Record(...)` per *visible* component at the top of its block,
   before `EditComponentFields`. (One serialize per visible component per frame on
   one entity — negligible.)
3. **Gizmo**: `Record(selected, Transform)` before `ImGuizmo::Manipulate` while
   `!IsUsing()`.
4. **Off-inspector sites** (each one `Record` line before the write): eyedropper
   (`ApplyEyedropperPick`), asset browser (`SelectAsset`), rename (opens the
   gesture *before* the Name component is created on first keystroke — the
   `optional before = nullopt` naturally records the create), add/remove component,
   entity create/delete.
5. **End-of-frame commit sweep**: for each open gesture, if its widget is no longer
   active (or it was an instant edit), serialize `after`, and **commit** a
   transaction — **unless `before == after`** (drop no-ops; this also absorbs
   Escape-revert and click-without-drag).

Commit-trigger reliability is **per-widget-class**, not one rule:

- **Drags / text inputs** → commit on `IsItemDeactivatedAfterEdit`.
- **Combos / checkboxes / selectables** (enum, EntityRef, asset picks) → the edit
  happens on a `Selectable` *inside a popup*, a different item than the combo →
  treat as an **instant one-frame transaction** (commit the frame `edited` fires).
- **Escape-revert**: `IsItemDeactivatedAfterEdit` returns true even on Escape —
  the `before == after` drop handles it.
- **Selection change / entity death mid-gesture** → abandon the open gesture
  (restore nothing, record nothing) or force-commit; never leave a half-open
  gesture whose falling edge can't pair.

Key the transaction boundary to the specific `(entity, componentId)` being
edited — **not** `IsAnyItemActive` (the existing `_wasDragging` code needed
window-focus hacks; don't repeat that).

## 6. Apply semantics (undo / redo)

- **Undo** replays a transaction's deltas toward `before`; **redo** toward `after`.
- **Order**: apply commands in **reverse** for undo, **forward** for redo.
- **Per ComponentDelta**: target `nullopt` ⇒ `Scene::RemoveById(entity, id)`
  (+ transient teardown, see rebind table); else **restore the JSON**. Note
  `addToScene` bottoms out in `Scene::Add`, which **rejects (silent no-op) when the
  component already exists** — so restore is **remove-first-then-add**, always.
  Debug-assert via `getByEntity` after apply.
- **Two-phase for multi-entity / subtree** (`EntityDelta` with children): **revive
  all entities first, then add all components** (`Scene::Add` needs the target
  entity alive). Refs *between* subtree members are just data (no ordering needed);
  the entity phase needs ordering. Finding children = scan the `Parent` pool
  (no child index) — O(n) per delete, fine at editor scale.
- **Transient-state rebuild table** (transient fields are excluded from serialize,
  so restored components come back with null/empty transients — rebuild them,
  routed through the *same* helpers the live edit paths use):
  | Component | Post-apply action |
  |---|---|
  | `Transform` | `SetBodyTransform` if the entity has a body (else nothing; `Add` already stamped the change tick → `PropagateTransforms` reruns) |
  | `RigidBodyDescriptor` | rebuild the Jolt body: `RigidBody` is `ACOMP(transient)` (never in the payload), so an add ⇒ `AddPhysicsBody`, a remove ⇒ `RemoveBody` + `Remove<RigidBody>` (exactly like `RemoveComponentFromSelected`) |
  | `MeshRenderer` | `ReresolveEntityAssets(entity)` (rebuilds `meshBuffer`/`materials` transient pointers) |
- **Selection**: restore `selectionBefore`/`selectionAfter` (Unreal does; and
  `ReviveAt` makes a stored `_selectedEntity` valid again after undoing a delete).
- **Re-entrancy guard**: apply calls `addToScene`/`RemoveById`/rebind — the same
  functions the capture hooks wrap. Set `_applying = true` around apply so applies
  never record.
- **Frame timing**: apply at **one fixed frame point** — queue the request from
  the Ctrl-Z handler, apply at the **top of `OnUpdate`** (after the previous
  frame's `FlushDeferred`). Never mid-ImGui (invalidates cached `compPtr`), never
  while `_pendingDestroy` is non-empty (§8.3).

## 7. New runtime primitives (the only shipping-module additions)

Both are **neutral capabilities**, not undo logic (also useful for
deserialization, copy/paste, prefab instantiation, netcode rollback/replay). Both
must be documented with their invariants.

### 7.1 `Scene::ReviveAt(Entity handle)` (+ internal `Registry::ReviveAt`)

Restore a freed slot to its **exact prior `(index, generation)`** so all existing
references stay valid with no scanning/patching.

```cpp
void Registry::ReviveAt(Entity e) {         // internal
    assert(e.index < _generations.size());
    assert(!_alive[e.index]);               // slot must be free
    _generations[e.index] = e.generation;   // exact identity (generation may DECREASE)
    _alive[e.index] = true;
    std::erase(_freeSlots, e.index);        // MUST scrub the free list (§8.2)
    ++_aliveCount;
}
```

- **Public surface is `Scene::ReviveAt`** (not raw Registry), so it can also police
  the deferred-destroy queue (`_pendingDestroy`) and re-register liveness.
- Used by **undo of delete** *and* **redo of create** (both need exact-handle
  restore, else later history entries referencing the handle go stale).
- **Invariant (document it):** valid only when no other live handle exists for that
  slot — i.e. under a **strictly linear editor history** (undo runs newest-first,
  so the slot is free when we resurrect; a new committed transaction clears the
  redo stack). Generations may rewind, which breaks the engine-wide
  monotonic-generation assumption — this is the *only* context where that's
  allowed. The `!_alive` assert catches most violations.

### 7.2 `SceneSerializer` scoped raw-entity (identity) context

**Blocking fix.** reflectgen's generated serialize/deserialize for any
`AFIELD() ECS::Entity` field (e.g. `Parent.parent`) routes through
`SceneSerializer`'s thread-local remap context, which is engaged **only inside
`Save`/`Load`**. Called per-delta from the editor (no context):

- serialize → `EntityToIndex(...)` returns `nullopt` → captures `parent = ~0u`;
- deserialize → `IndexToEntity(...)` returns `NullEntity` → **silently unparents**.

So without this, every undo/redo silently flattens the hierarchy, no error.

Add a **public RAII scope** (e.g. `SceneSerializer::ScopedRawEntityContext`) in
which `EntityToIndex(e)` returns `e.index` and `IndexToEntity(i)` returns
`scene.EntityAt(i)`. Under `ReviveAt` (exact index+generation), `EntityAt(index)`
resolves correctly at apply time. Wrap **every** history capture and apply in it.

**Documented asymmetry:** level files use remapped serial indices (correct for
save/load); undo payloads use raw handles (correct only because of `ReviveAt`).

## 8. Correctness requirements / edge cases (from the review, all verified)

1. **EntityRef serialization context** — §7.2. Blocking.
2. **`ReviveAt` free-list scrub** — §7.1; without it a later `Create()` hands out a
   duplicate live handle sharing all components. Silent corruption.
3. **Deferred destroy vs. undo** — `Scene::Destroy` only queues; `FlushDestroyed`
   runs end-of-frame. Undo in the same frame as a delete trips the `!_alive`
   assert; a revived entity still in `_pendingDestroy` gets re-killed by the flush.
   Fix: apply at top of `OnUpdate` (§6); `Scene::ReviveAt` also erases the handle
   from `_pendingDestroy`.
4. **Play mode / level load wipe identity** — a naive `Save`/`Load` restore
   rebuilds entities densely from `{0,0}`; stored handles dangle *and can alias* a
   different entity at the same slot (corruption, no assert). **Level load** still
   clears the history (a genuinely new scene). **Play**, however, keeps a richer
   contract (implemented Stage 2, refining the original "clear on Stop"):
   - **Editing** — the persistent main history is active.
   - **Playing** — no history active; capture/apply frozen (physics owns Transforms).
   - **Paused** — a *separate scratch history* is active; edits made while paused are
     undoable there, but the whole container is discarded when play resumes or
     stops (paused undo never leaks into the editing history).
   - **The editing history survives a play session.** `StartPlay` snapshots each
     entity's *exact* `(index, generation)` + component JSON (raw-entity context);
     `StopPlay` tears the scene down (Destroy+flush, **not** `Scene::Clear`, so the
     slot table survives) and rebuilds it via `ReviveAt` at those exact handles.
     Because identity is preserved, the pre-play history's stored handles stay
     valid: **edit → play → stop → undo works.** This is the §8.4/§11 "route Stop
     through `ReviveAt`-exact restore" item — now built, not deferred.
5. **Capture-before-write** — §5; post-hoc capture reads already-modified memory.
6. **Commit triggers per widget** — §5; drags vs combos/checkboxes differ; drop
   `before == after`.
7. **Edit sites that bypass the inspector** (the "track ALL edits" risk list):
   eyedropper (`ApplyEyedropperPick`, raw-offset Entity write), asset browser
   (`SelectAsset`, raw-offset AssetId/vector write), rename (creates Name on first
   keystroke), add-component **side effects** (camera-facing Transform placement +
   physics body creation happen *after* `addToScene` — capture `after` **after**
   the side effects). Entity delete does **not exist yet** — build it with its undo.
8. **`addToScene` remove-first** — §6; silent no-op if the component exists.
9. **RigidBody transient rebuild** — §6 table; `RigidBody` is `ACOMP(transient)`.
10. **Selection in transaction** — §4/§6.
11. **Re-entrancy guard** (`_applying`) — §6.
12. **Ctrl-Z inside text fields** — ImGui InputText has its own internal undo; gate
    scene undo on `!ImGui::GetIO().WantTextInput` (codebase idiom:
    `ImGuiWantsKeyboard()`).
13. **Redo needs `ReviveAt` too** — §7.1; generations can decrease.
14. **Memory cap + redo-stack clear on new commit** — §4; the linear-history
    invariant that all of §7.1's safety rests on.
15. **Debug divergence checker** — on commit (occasionally), assert
    `Save(scene) == replay(baseline + history)`. The only mechanism that *proves*
    no edit path is silently missed (the stated goal).

## 9. Editor-only boundary

- The **entire undo system** — `EditHistory`, the variant deltas, all capture/apply
  and rebind wiring — lives in **`apps/sandbox`** (the editor app). A shipped game
  is a different app target that never compiles sandbox sources → excluded **by
  construction**, no macro, no dead code. (Cleaner than the current precedent where
  `AssetDatabase`/reconcile sit in `Core`, only *documented* as editor-only.)
- The **only** shipping-module additions are the two neutral primitives in §7
  (`Scene::ReviveAt`, `SceneSerializer` identity context). Both documented with
  invariants; both justifiable on non-editor merits.

## 10. Staged rollout

Each stage builds + runs; the editor stays usable throughout.

- **Stage 1 — foundations.** `Scene::ReviveAt` + `Registry::ReviveAt` (+ tests:
  destroy→revive round-trips exact handle, references survive, free-list scrubbed,
  deferred-destroy interaction). `SceneSerializer` raw-entity identity scope
  (+ test: Parent round-trips a raw handle outside Save/Load). `EditHistory`
  container + `EditCommand` variant + apply engine (reverse/forward, two-phase,
  rebind table, `_applying` guard) with a couple of hand-built transactions
  exercised in a unit-ish test.
- **Stage 2 — capture wiring. DONE (2026-07-16).** Record-before-write at all edit
  sites: inspector (pre-draw snapshot per visible component + end-of-frame commit
  sweep), gizmo, eyedropper, `SelectAsset`, rename, add/remove component. Coarse
  commit trigger (an inspector-scoped "any widget active" / gizmo `IsUsing` signal
  keeps a gesture open; it commits on release), no-op `before == after` drop.
  Hotkeys (Ctrl-Z / Ctrl-Y / Ctrl-Shift-Z, gated on `!WantTextInput`), applied at
  top of `OnUpdate`. Play-state contract per §8.4 (main history in Editing, scratch
  history in Paused discarded on resume/stop, none while Playing; editing history
  survives play via exact-identity Stop restore). History cleared on level load.
  *Remaining polish:* the debug divergence checker (Save-hash tripwire for a missed
  edit site) — deferred to keep this stage focused; low-risk follow-up.
- **Stage 3 — entity lifetime + polish. DONE (2026-07-16).** Entity create
  (`CreateEntity` → `EntityDelta`) and delete (`DeleteEntity`: subtree gathered via
  the `Parent` pool, snapshotted, physics torn down, destroyed — one transaction;
  Delete key + a `-` button in the Entities window). History panel (Photoshop-style
  linear view, click a row to jump; jumps deferred to `OnUpdate` top). Depth cap
  (Stage 1). Modified-`*` marker in the window title, via a per-transaction state
  token recorded at `SaveLevel`. Deletion/create both ride the Stage-1 apply engine
  (two-phase `ReviveAt`), so no new core logic — only capture + UI.

## 11. Deferred / open

- ~~Edit-mode undo surviving Play (§8.4)~~ — **built in Stage 2** via exact-identity
  (`ReviveAt`) Stop restore + a scratch pause history.
- Debug divergence checker (§8.15) — the one Stage 2 item not yet built.
- Transaction merge policy for consecutive same-`(entity, component, label)` edits
  in a short window (Unity's `CollapseUndoOperations`) — escape hatch if
  per-deactivation commits make text editing a per-keystroke Ctrl-Z crawl.
- Asset-level ops (`.amat` regenerate/reimport) staying out of scene undo — decided
  out of scope; revisit if it feels wrong in use.

## Related

- `docs/asset-database-architecture.md` — the editor/shipped split this respects.
- reflectgen (`ComponentMeta::serialize`/`addToScene`/`getByEntity`, `Scene::RemoveById`).
- `modules/Runtime/src/SceneSerializer.cpp` — the remap-context coupling §7.2 fixes.
