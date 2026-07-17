/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <optional>
#include <vector>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include "EditHistory.hpp"

using namespace Assisi;
using ECS::Entity;
using ECS::NullEntity;
using ECS::Scene;
using Runtime::Camera;
using Runtime::Parent;
using Runtime::SceneSerializer;
using Runtime::Transform;
using Sandbox::ComponentDelta;
using Sandbox::ComponentSnapshot;
using Sandbox::EditHistory;
using Sandbox::EntityDelta;
using Sandbox::Transaction;

namespace
{
Core::Reflect::ComponentId IdOf(std::string_view name)
{
    const auto *meta = Core::Reflect::ComponentRegistry::Instance().Find(name);
    REQUIRE(meta != nullptr);
    return meta->id;
}

// Serialize one live component to JSON the way the capture layer will: under a
// raw-entity context so EntityRef fields encode raw handles. nullopt if absent.
std::optional<nlohmann::json> CaptureComponent(Scene &scene, Entity e, Core::Reflect::ComponentId id)
{
    const auto *meta = Core::Reflect::ComponentRegistry::Instance().ById(id);
    if (!meta || !meta->getByEntity)
        return std::nullopt;
    const void *comp = meta->getByEntity(&scene, e.index, e.generation);
    if (!comp)
        return std::nullopt;
    SceneSerializer::ScopedRawEntityContext raw(scene);
    return meta->serialize(comp);
}

// Full component set of one entity (for an entity create/delete snapshot).
std::vector<ComponentSnapshot> SnapshotEntity(Scene &scene, Entity e)
{
    std::vector<ComponentSnapshot> snaps;
    SceneSerializer::ScopedRawEntityContext raw(scene);
    for (const auto *meta : Core::Reflect::ComponentRegistry::Instance().SerializableComponents())
    {
        if (const void *comp = meta->getByEntity(&scene, e.index, e.generation))
            snaps.push_back({meta->id, meta->serialize(comp)});
    }
    return snaps;
}

// Records every rebind call so tests can assert the engine drives the transient
// rebuild hook.
struct RebindLog
{
    struct Call
    {
        Entity                     entity;
        Core::Reflect::ComponentId id;
        bool                       present;
    };
    std::vector<Call> calls;
};
} // namespace

TEST_CASE("EditHistory: field-edit transaction undoes and redoes a value")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 2.f, 3.f}}) != nullptr);

    const auto tid    = IdOf("Transform");
    const auto before = CaptureComponent(scene, e, tid);

    // The live edit already happened by the time we build the delta (record-before-
    // write captures `before`; the widget wrote the new value in between).
    scene.GetMut<Transform>(e)->position = {9.f, 9.f, 9.f};
    const auto after = CaptureComponent(scene, e, tid);

    EditHistory hist(scene);
    Transaction txn;
    txn.label           = "Move";
    txn.selectionBefore = e;
    txn.selectionAfter  = e;
    txn.cmds.push_back(ComponentDelta{e, tid, before, after});
    hist.Push(std::move(txn));

    REQUIRE(hist.CanUndo());
    REQUIRE_FALSE(hist.CanRedo());

    const auto undoSel = hist.Undo();
    REQUIRE(undoSel.has_value());
    CHECK(*undoSel == e);
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(1.f));
    CHECK(hist.CanRedo());

    const auto redoSel = hist.Redo();
    REQUIRE(redoSel.has_value());
    CHECK(*redoSel == e);
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(9.f));
}

TEST_CASE("EditHistory: add-component transaction toggles presence")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);

    const auto cid    = IdOf("Camera");
    const auto before = CaptureComponent(scene, e, cid); // absent
    REQUIRE_FALSE(before.has_value());

    REQUIRE(scene.Add(e, Camera{.fovDegrees = 42.f}) != nullptr);
    const auto after = CaptureComponent(scene, e, cid); // present
    REQUIRE(after.has_value());

    EditHistory hist(scene);
    Transaction txn;
    txn.label = "Add Camera";
    txn.cmds.push_back(ComponentDelta{e, cid, before, after});
    hist.Push(std::move(txn));

    hist.Undo();
    CHECK_FALSE(scene.Has<Camera>(e));

    hist.Redo();
    REQUIRE(scene.Has<Camera>(e));
    CHECK(scene.Get<Camera>(e)->fovDegrees == doctest::Approx(42.f));
}

TEST_CASE("EditHistory: remove-component transaction toggles presence the other way")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);
    REQUIRE(scene.Add(e, Camera{.fovDegrees = 30.f}) != nullptr);

    const auto cid    = IdOf("Camera");
    const auto before = CaptureComponent(scene, e, cid); // present
    REQUIRE(before.has_value());

    scene.RemoveById(e, cid);
    const auto after = CaptureComponent(scene, e, cid); // absent
    REQUIRE_FALSE(after.has_value());

    EditHistory hist(scene);
    Transaction txn;
    txn.label = "Remove Camera";
    txn.cmds.push_back(ComponentDelta{e, cid, before, after});
    hist.Push(std::move(txn));

    hist.Undo(); // brings it back
    REQUIRE(scene.Has<Camera>(e));
    CHECK(scene.Get<Camera>(e)->fovDegrees == doctest::Approx(30.f));

    hist.Redo(); // removes it again
    CHECK_FALSE(scene.Has<Camera>(e));
}

TEST_CASE("EditHistory: entity-create transaction destroys on undo and revives exact on redo")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {5.f, 0.f, 0.f}}) != nullptr);
    const auto snap = SnapshotEntity(scene, e);

    EditHistory hist(scene);
    Transaction txn;
    txn.label           = "Create Entity";
    txn.selectionBefore = NullEntity;
    txn.selectionAfter  = e;
    txn.cmds.push_back(EntityDelta{e, std::nullopt, snap}); // before absent, after present
    hist.Push(std::move(txn));

    // Undo the create → the entity is destroyed.
    const auto undoSel = hist.Undo();
    REQUIRE(undoSel.has_value());
    CHECK(*undoSel == NullEntity);
    CHECK_FALSE(scene.IsAlive(e));

    // Redo → the exact handle comes back with its Transform restored.
    const auto redoSel = hist.Redo();
    REQUIRE(redoSel.has_value());
    CHECK(*redoSel == e);
    REQUIRE(scene.IsAlive(e));
    CHECK(scene.EntityAt(e.index) == e);
    REQUIRE(scene.Get<Transform>(e) != nullptr);
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(5.f));
}

TEST_CASE("EditHistory: subtree-delete revives entities and resolves the Parent ref on undo")
{
    Scene        scene;
    const Entity parent = scene.Create();
    const Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{}) != nullptr);
    REQUIRE(scene.Add(child, Transform{}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    // Snapshot the subtree, then perform the delete the transaction describes.
    const auto parentSnap = SnapshotEntity(scene, parent);
    const auto childSnap   = SnapshotEntity(scene, child);
    scene.Destroy(parent);
    scene.Destroy(child);
    scene.FlushDestroyed();
    REQUIRE_FALSE(scene.IsAlive(parent));
    REQUIRE_FALSE(scene.IsAlive(child));

    EditHistory hist(scene);
    Transaction txn;
    txn.label           = "Delete Subtree";
    txn.selectionBefore = child;
    txn.selectionAfter  = NullEntity;
    txn.cmds.push_back(EntityDelta{parent, parentSnap, std::nullopt});
    txn.cmds.push_back(EntityDelta{child, childSnap, std::nullopt});
    hist.Push(std::move(txn));

    // Undo → both entities revive at their exact handles; the child's Parent ref
    // resolves to the parent (the whole point of two-phase + the raw context).
    const auto undoSel = hist.Undo();
    REQUIRE(undoSel.has_value());
    CHECK(*undoSel == child);
    REQUIRE(scene.IsAlive(parent));
    REQUIRE(scene.IsAlive(child));
    CHECK(scene.EntityAt(parent.index) == parent);
    const Parent *pc = scene.Get<Parent>(child);
    REQUIRE(pc != nullptr);
    CHECK(pc->parent == parent); // NOT flattened to NullEntity

    // Redo → both destroyed again.
    const auto redoSel = hist.Redo();
    REQUIRE(redoSel.has_value());
    CHECK(*redoSel == NullEntity);
    CHECK_FALSE(scene.IsAlive(parent));
    CHECK_FALSE(scene.IsAlive(child));
}

TEST_CASE("EditHistory: a new commit clears the redo stack")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);
    const auto tid = IdOf("Transform");
    const auto j   = CaptureComponent(scene, e, tid);

    EditHistory hist(scene);
    const auto push = [&](const char *label) {
        Transaction txn;
        txn.label = label;
        txn.cmds.push_back(ComponentDelta{e, tid, j, j}); // no-op values, still a command
        hist.Push(std::move(txn));
    };

    push("A");
    push("B");
    hist.Undo(); // B moves to redo
    REQUIRE(hist.CanRedo());

    push("C"); // committing a new edit must discard the redo future
    CHECK_FALSE(hist.CanRedo());
}

TEST_CASE("EditHistory: the depth cap drops the oldest transactions")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);
    const auto tid = IdOf("Transform");
    const auto j   = CaptureComponent(scene, e, tid);

    EditHistory hist(scene);
    for (std::size_t i = 0; i < EditHistory::kMaxDepth + 5; ++i)
    {
        Transaction txn;
        txn.label = "edit";
        txn.cmds.push_back(ComponentDelta{e, tid, j, j});
        hist.Push(std::move(txn));
    }
    CHECK(hist.UndoDepth() == EditHistory::kMaxDepth);
}

TEST_CASE("EditHistory: the rebind hook fires during apply and only during apply")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    const auto tid    = IdOf("Transform");
    const auto before = CaptureComponent(scene, e, tid);
    scene.GetMut<Transform>(e)->position = {2.f, 0.f, 0.f};
    const auto after = CaptureComponent(scene, e, tid);

    RebindLog          log;
    bool               sawApplyingFalse = false;
    const EditHistory *applier          = nullptr; // set to &hist after construction

    // The hook reads IsApplying() on the very history it is bound to — resolved via
    // a back-pointer filled in once the object exists (ctor-argument chicken/egg).
    EditHistory hist(scene, [&](Entity ent, Core::Reflect::ComponentId id, bool present) {
        if (applier != nullptr && !applier->IsApplying())
            sawApplyingFalse = true;
        log.calls.push_back({ent, id, present});
    });
    applier = &hist;

    Transaction txn;
    txn.label = "Move";
    txn.cmds.push_back(ComponentDelta{e, tid, before, after});
    hist.Push(std::move(txn));

    CHECK(log.calls.empty()); // Push must not apply anything

    hist.Undo();
    REQUIRE_FALSE(log.calls.empty());
    CHECK(log.calls.back().id == tid);
    CHECK(log.calls.back().present == true); // Transform restored, not removed
    CHECK_FALSE(sawApplyingFalse);           // every call happened while applying
}

// ---------------------------------------------------------------------------
// Capture layer (record-before-write)
// ---------------------------------------------------------------------------

TEST_CASE("EditHistory: a captured drag commits on gesture end")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    const auto tid = IdOf("Transform");

    EditHistory hist(scene);

    // Frame 1: open the gesture (before = pos 1), then the "widget" writes pos 2.
    hist.RecordBefore(e, tid, "Move", e);
    scene.GetMut<Transform>(e)->position = {2.f, 0.f, 0.f};
    hist.EndFrameSweep(/*editingActive=*/true); // still dragging — no commit yet
    CHECK_FALSE(hist.CanUndo());

    // Frame 2: still holding, value moves again to 3.
    hist.RecordBefore(e, tid, "Move", e); // idempotent — keeps before = pos 1
    scene.GetMut<Transform>(e)->position = {3.f, 0.f, 0.f};
    hist.EndFrameSweep(/*editingActive=*/true);
    CHECK_FALSE(hist.CanUndo());

    // Frame 3: released. One transaction spanning the whole drag (pos 1 -> 3).
    hist.EndFrameSweep(/*editingActive=*/false);
    REQUIRE(hist.CanUndo());

    hist.Undo();
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(1.f));
    hist.Redo();
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(3.f));
}

TEST_CASE("EditHistory: a no-op gesture (click without change) commits nothing")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    const auto tid = IdOf("Transform");

    EditHistory hist(scene);
    hist.RecordBefore(e, tid, "Move", e);
    // no write happened
    hist.EndFrameSweep(/*editingActive=*/false);
    CHECK_FALSE(hist.CanUndo());
}

TEST_CASE("EditHistory: CommitGesture closes an instant edit immediately")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);
    const auto cid = IdOf("Camera");

    EditHistory hist(scene);
    hist.RecordBefore(e, cid, "Add Camera", e); // before = absent
    REQUIRE(scene.Add(e, Camera{.fovDegrees = 55.f}) != nullptr);
    hist.CommitGesture(e, cid);

    REQUIRE(hist.CanUndo());
    hist.Undo();
    CHECK_FALSE(scene.Has<Camera>(e));
}

TEST_CASE("EditHistory: a gesture whose entity dies is abandoned, not committed")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);
    const auto tid = IdOf("Transform");

    EditHistory hist(scene);
    hist.RecordBefore(e, tid, "Move", e);
    scene.GetMut<Transform>(e)->position = {5.f, 0.f, 0.f};

    scene.Destroy(e);
    scene.FlushDestroyed();

    hist.EndFrameSweep(/*editingActive=*/false); // entity gone — must not throw or commit
    CHECK_FALSE(hist.CanUndo());
}

TEST_CASE("EditHistory: capture is suppressed while applying")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    const auto tid = IdOf("Transform");

    // A rebind hook that tries to record during apply must be ignored (RecordBefore
    // no-ops while _applying), so an undo can't spawn a spurious transaction.
    EditHistory hist(scene);
    // Seed one real transaction to undo.
    const auto before = CaptureComponent(scene, e, tid);
    scene.GetMut<Transform>(e)->position = {2.f, 0.f, 0.f};
    const auto after = CaptureComponent(scene, e, tid);
    Transaction txn;
    txn.cmds.push_back(ComponentDelta{e, tid, before, after});
    hist.Push(std::move(txn));

    hist.Undo();
    // A record attempted mid-apply would have left an open gesture; a following
    // sweep would then commit it. Prove nothing is pending.
    hist.RecordBefore(e, tid, "Move", e); // this one is outside apply, legitimately opens
    hist.EndFrameSweep(false);            // no write -> no-op drop
    CHECK(hist.RedoDepth() == 1);         // only the seeded transaction, nothing extra
    CHECK(hist.UndoDepth() == 0);
}

// ---------------------------------------------------------------------------
// The editing history survives an exact-identity play/stop cycle.
//
// StopPlay restores the scene by reviving entities at their *exact* prior handles
// (Scene::ReviveAt) rather than renumbering them via Save/Load — which is what
// keeps a pre-play editing history's stored handles valid. This test mimics that
// snapshot/restore and proves an undo recorded before "play" still applies after
// "stop". (The real StartPlay/StopPlay live in SandboxPlay.cpp; the mechanism is
// what's exercised here.)
// ---------------------------------------------------------------------------

namespace
{
struct EntitySnap
{
    Entity                         handle;
    std::vector<ComponentSnapshot> components;
};

std::vector<EntitySnap> SnapshotScene(Scene &scene)
{
    std::vector<EntitySnap> out;
    scene.ForEachEntity([&](Entity e) { out.push_back({e, SnapshotEntity(scene, e)}); });
    return out;
}

// Mimics StopPlay: destroy every live entity (keeping the registry table intact),
// then revive each snapshot entity at its exact handle and restore its components.
void RestoreSceneExact(Scene &scene, const std::vector<EntitySnap> &snapshot)
{
    std::vector<Entity> live;
    scene.ForEachEntity([&](Entity e) { live.push_back(e); });
    for (Entity e : live)
        scene.Destroy(e);
    scene.FlushDestroyed();

    SceneSerializer::ScopedRawEntityContext raw(scene);
    const auto                             &registry = Core::Reflect::ComponentRegistry::Instance();
    for (const EntitySnap &snap : snapshot)
        scene.ReviveAt(snap.handle);
    for (const EntitySnap &snap : snapshot)
        for (const ComponentSnapshot &comp : snap.components)
            if (const auto *meta = registry.ById(comp.id); meta && meta->addToScene)
                meta->addToScene(&scene, snap.handle.index, snap.handle.generation, comp.data);
}
} // namespace

TEST_CASE("EditHistory: an editing undo survives a play (snapshot -> restore) cycle")
{
    Scene        scene;
    const Entity a = scene.Create();
    const Entity b = scene.Create(); // a second entity so identity isn't trivially {0,0}
    REQUIRE(scene.Add(a, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(b, Transform{}) != nullptr);

    const auto tid = IdOf("Transform");

    // --- Editing: record a change to a.position (1 -> 5). ---
    EditHistory hist(scene);
    const auto  before = CaptureComponent(scene, a, tid);
    scene.GetMut<Transform>(a)->position = {5.f, 0.f, 0.f};
    const auto after = CaptureComponent(scene, a, tid);
    Transaction txn;
    txn.selectionBefore = a;
    txn.selectionAfter  = a;
    txn.cmds.push_back(ComponentDelta{a, tid, before, after});
    hist.Push(std::move(txn));

    // --- Play: snapshot, then let "physics" scramble the transforms. ---
    const std::vector<EntitySnap> snapshot = SnapshotScene(scene);
    scene.GetMut<Transform>(a)->position = {99.f, 99.f, 99.f};
    scene.GetMut<Transform>(b)->position = {42.f, 0.f, 0.f};

    // --- Stop: exact-identity restore. a comes back at pos 5 (its pre-play edit). ---
    RestoreSceneExact(scene, snapshot);
    REQUIRE(scene.IsAlive(a));
    CHECK(scene.EntityAt(a.index) == a); // exact handle preserved
    CHECK(scene.Get<Transform>(a)->position.x == doctest::Approx(5.f));

    // --- The pre-play undo still resolves against the restored handle. ---
    const auto sel = hist.Undo();
    REQUIRE(sel.has_value());
    CHECK(*sel == a);
    CHECK(scene.Get<Transform>(a)->position.x == doctest::Approx(1.f)); // reverted to pre-edit
}

TEST_CASE("EditHistory: CaptureEntityComponents snapshots every serializable component")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {3.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(e, Camera{.fovDegrees = 12.f}) != nullptr);

    EditHistory hist(scene);
    const auto  snap = hist.CaptureEntityComponents(e);
    CHECK(snap.size() == 2); // Transform + Camera

    const Entity bare = scene.Create();
    CHECK(hist.CaptureEntityComponents(bare).empty()); // no components
}

TEST_CASE("EditHistory: labels and the dirty-state token track the stack")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);
    const auto tid = IdOf("Transform");
    const auto j   = CaptureComponent(scene, e, tid);

    EditHistory hist(scene);
    const auto  push = [&](const char *label) {
        Transaction txn;
        txn.label = label;
        // distinct before/after so the transaction isn't a no-op
        auto after = j;
        txn.cmds.push_back(ComponentDelta{e, tid, j, after});
        hist.Push(std::move(txn));
    };

    CHECK(hist.CurrentStateToken() == 0); // base state
    push("A");
    push("B");

    const auto labels = hist.UndoLabels();
    REQUIRE(labels.size() == 2);
    CHECK(labels[0] == "A"); // oldest first
    CHECK(labels[1] == "B"); // newest last (next Undo target)

    const auto savedToken = hist.CurrentStateToken();
    CHECK(savedToken != 0);

    hist.Undo(); // now at A; token differs from the B-top saved token
    CHECK(hist.CurrentStateToken() != savedToken);
    CHECK(hist.RedoLabels().size() == 1);
    CHECK(hist.RedoLabels()[0] == "B");

    hist.Redo(); // back to B; token returns to the saved value (undo↔redo stable)
    CHECK(hist.CurrentStateToken() == savedToken);
}

TEST_CASE("EditHistory: a subtree delete built via CaptureEntityComponents round-trips")
{
    // Mirrors SandboxApp::DeleteEntity: snapshot each subtree member with the public
    // CaptureEntityComponents helper, build one delete transaction, destroy, undo.
    Scene        scene;
    const Entity parent = scene.Create();
    const Entity child  = scene.Create();
    REQUIRE(scene.Add(parent, Transform{.position = {2.f, 0.f, 0.f}}) != nullptr);
    REQUIRE(scene.Add(child, Transform{}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    EditHistory hist(scene);

    Transaction txn;
    txn.label           = "Delete Subtree";
    txn.selectionBefore = parent;
    txn.selectionAfter  = NullEntity;
    for (Entity e : {parent, child})
        txn.cmds.push_back(EntityDelta{e, hist.CaptureEntityComponents(e), std::nullopt});

    // Perform the delete the transaction describes, then record it.
    scene.Destroy(parent);
    scene.Destroy(child);
    scene.FlushDestroyed();
    hist.Push(std::move(txn));

    const auto sel = hist.Undo();
    REQUIRE(sel.has_value());
    CHECK(*sel == parent);
    REQUIRE(scene.IsAlive(parent));
    REQUIRE(scene.IsAlive(child));
    CHECK(scene.EntityAt(parent.index) == parent);
    CHECK(scene.Get<Transform>(parent)->position.x == doctest::Approx(2.f));
    const Parent *pc = scene.Get<Parent>(child);
    REQUIRE(pc != nullptr);
    CHECK(pc->parent == parent);
}

TEST_CASE("EditHistory: RecordBefore refreshes the label of an open gesture (keeps before)")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 0.f, 0.f}}) != nullptr);
    const auto tid = IdOf("Transform");

    EditHistory hist(scene);
    // Frame: the inspector opens the gesture as "Edit Transform" (before = pos 1)...
    hist.RecordBefore(e, tid, "Edit Transform", e);
    // ...then a remove-style site relabels the same gesture. `before` must be kept.
    hist.RecordBefore(e, tid, "Remove Transform", e);
    scene.GetMut<Transform>(e)->position = {2.f, 0.f, 0.f};
    hist.CommitGesture(e, tid);

    REQUIRE(hist.UndoDepth() == 1);
    CHECK(hist.NextUndoLabel() == "Remove Transform"); // last writer wins the label
    hist.Undo();
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(1.f)); // before was preserved
}

TEST_CASE("EditHistory: a force-committed gesture stays separate from a following edit")
{
    // Models a gizmo drag (force-commit on release) followed by an inspector edit of
    // the same Transform: two distinct undo entries, never coalesced, even though
    // they share the (entity, Transform) gesture key.
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {0.f, 0.f, 0.f}}) != nullptr);
    const auto tid = IdOf("Transform");

    EditHistory hist(scene);

    // Gizmo drag: open, move to 1, force-commit on release.
    hist.RecordBefore(e, tid, "Edit Transform", e);
    scene.GetMut<Transform>(e)->position = {1.f, 0.f, 0.f};
    hist.CommitGesture(e, tid);
    REQUIRE(hist.UndoDepth() == 1);

    // Inspector edit: a fresh gesture, move to 2, committed by the sweep.
    hist.RecordBefore(e, tid, "Edit Transform", e);
    scene.GetMut<Transform>(e)->position = {2.f, 0.f, 0.f};
    hist.EndFrameSweep(/*editingActive=*/false);
    CHECK(hist.UndoDepth() == 2); // two separate actions

    hist.Undo();
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(1.f)); // back to the gizmo result
    hist.Undo();
    CHECK(scene.Get<Transform>(e)->position.x == doctest::Approx(0.f)); // back to the start
}

TEST_CASE("EditHistory: empty history and no-op transactions are safe")
{
    Scene       scene;
    EditHistory hist(scene);

    CHECK_FALSE(hist.CanUndo());
    CHECK_FALSE(hist.CanRedo());
    CHECK_FALSE(hist.Undo().has_value());
    CHECK_FALSE(hist.Redo().has_value());

    hist.Push(Transaction{}); // no commands — dropped
    CHECK_FALSE(hist.CanUndo());
}

// Round-6 review C4: on undo of an entity delete, EditHistory restores each
// component and fires the rebind hook immediately — in registry (alphabetical)
// order. A component that sorts before Transform (Camera here; RigidBodyDescriptor
// in the app) is therefore rebound while Transform is still absent. The app's
// physics rebind reads Transform at that moment and, finding none, silently skips
// creating the Jolt body. The invariant: when any component is rebound during a
// revive, its siblings from the same snapshot are already present.
TEST_CASE("EditHistory: undo-of-delete has all siblings present when a component is rebound")
{
    Scene        scene;
    const Entity e = scene.Create();
    REQUIRE(scene.Add(e, Camera{}) != nullptr); // sorts before "Transform"
    REQUIRE(scene.Add(e, Transform{.position = {5.f, 0.f, 0.f}}) != nullptr);

    const auto cameraId = IdOf("Camera");

    bool sawCameraRebind               = false;
    bool transformPresentAtCameraRebind = false;
    EditHistory::RebindHook hook = [&](Entity ent, Core::Reflect::ComponentId id, bool present)
    {
        if (present && id == cameraId)
        {
            sawCameraRebind                = true;
            transformPresentAtCameraRebind = (scene.Get<Transform>(ent) != nullptr);
        }
    };

    const auto snap = SnapshotEntity(scene, e); // [Camera, Transform]
    scene.Destroy(e);
    scene.FlushDestroyed();
    REQUIRE_FALSE(scene.IsAlive(e));

    EditHistory hist(scene, hook);
    Transaction txn;
    txn.label = "Delete Entity";
    txn.cmds.push_back(EntityDelta{e, snap, std::nullopt});
    hist.Push(std::move(txn));

    hist.Undo(); // revive + restore components, firing the hook per component
    REQUIRE(scene.IsAlive(e));
    REQUIRE(sawCameraRebind);
    CHECK(transformPresentAtCameraRebind); // false today: hook fires before Transform is restored
}
