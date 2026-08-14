/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestGizmoDrag.cpp
/// @brief A gizmo drag is its own transaction, over its own entities, and it ends
/// where it ends.
///
/// The failure this exists to prevent is an **orphaned release edge** (ENG-127).
/// The gizmo's force-commit used to sit at the bottom of the draw function, past
/// four early returns — instance mode, a dead or non-editable entity, a Transform
/// that went away. Ending a drag through any of them skipped the commit and left
/// the "was dragging" flag raised, so the next frame that *did* reach the bottom
/// read that stale edge and committed it against whatever was selected by then:
/// a commit fired at an entity the drag never touched.
///
/// The same function rebuilt the also-dragged set from the live selection on every
/// frame, so even a well-formed release could name entities that were not part of
/// the drag.
///
/// So the assertions that matter are about *whose* edit gets committed and when —
/// not merely that something lands in the history.

#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <vector>

#include <Assisi/Core/Reflect/ComponentId.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Editor/GizmoDrag.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;
using Assisi::ECS::Entity;
using Assisi::Editor::EditHistory;
using Assisi::Editor::GizmoDrag;
using Assisi::Runtime::Transform;

namespace
{

/// A scene, a history bound to it, and the drag under test — the three pieces
/// DrawTransformGizmo has in hand every frame.
struct Fixture
{
    Fixture() : history(scene) {}

    Entity Spawn(float x)
    {
        const Entity entity = scene.Create();
        REQUIRE(scene.Add(entity, Transform{.position = {x, 0.f, 0.f}}) != nullptr);
        return entity;
    }

    void Nudge(Entity entity, float dx)
    {
        if (Transform *transform = scene.GetMut<Transform>(entity))
            transform->position.x += dx;
    }

    [[nodiscard]] float X(Entity entity) { return scene.Get<Transform>(entity)->position.x; }

    /// One frame with the handles held, in the order the draw code runs it: record
    /// before writing, write, and report the hold. The drag itself moves nothing —
    /// it only decides when what moved becomes an undo entry — so the test moves
    /// the scene the same way the gizmo does.
    void HeldFrame(Entity handle, std::span<const Entity> alsoDragged, float dx)
    {
        history.RecordBefore(handle, kTransformId, "Edit Transform", handle);
        Nudge(handle, dx);
        for (const Entity entity : alsoDragged)
        {
            history.RecordBefore(entity, kTransformId, "Edit Transform", handle);
            Nudge(entity, dx);
        }
        drag.Hold(handle, alsoDragged);
    }

    /// A frame in which the handles are not held. Crucially this includes every
    /// frame the gizmo early-returned and drew nothing at all: the release is
    /// driven from outside those returns, which is the whole point.
    void ReleasedFrame() { drag.Release(&scene, &history, kTransformId); }

    const Assisi::Core::Reflect::ComponentId kTransformId =
        Assisi::Core::Reflect::ComponentIdOf<Transform>();

    ECS::Scene scene;
    EditHistory history;
    GizmoDrag drag;
};

} // namespace

TEST_CASE("GizmoDrag: a multi-frame drag commits once per entity on release")
{
    Fixture fixture;
    const Entity handle = fixture.Spawn(0.f);
    const Entity rider  = fixture.Spawn(4.f);
    const std::vector<Entity> riders{rider};

    // Half a second of dragging both. The history is keyed by (entity, component),
    // so two entities moving together are two gestures — but they open and close on
    // the same edges, which is what makes it one drag to the person doing it.
    constexpr int32_t kFrames = 30;
    for (int32_t frame = 0; frame < kFrames; ++frame)
        fixture.HeldFrame(handle, riders, 0.1f);

    // Nothing yet: the drag has not finished, so there is no completed edit.
    CHECK(fixture.history.UndoDepth() == 0);
    CHECK(fixture.drag.IsOpen());

    fixture.ReleasedFrame();

    CHECK_FALSE(fixture.drag.IsOpen());
    CHECK(fixture.history.UndoDepth() == 2);

    // And each is the *whole* drag, not its last frame.
    CHECK(fixture.X(handle) == doctest::Approx(3.f));
    (void)fixture.history.Undo();
    (void)fixture.history.Undo();
    CHECK(fixture.X(handle) == doctest::Approx(0.f));
    CHECK(fixture.X(rider) == doctest::Approx(4.f));

    // The edge is spent. Releasing again — the editor does it on every idle frame —
    // must not commit the drag a second time.
    fixture.ReleasedFrame();
    CHECK(fixture.history.UndoDepth() == 0);
}

TEST_CASE("GizmoDrag: the entity set belongs to the drag, not to the live selection")
{
    Fixture fixture;
    const Entity dragged = fixture.Spawn(0.f);
    const Entity other   = fixture.Spawn(5.f);

    for (int32_t frame = 0; frame < 10; ++frame)
        fixture.HeldFrame(dragged, {}, 0.1f);

    // The selection moves to another entity while the handles are still held —
    // clicking a row in the entity list does not need the mouse to leave the gizmo.
    // The drag keeps the entities it grabbed at the press edge.
    fixture.drag.Hold(other, {});
    REQUIRE(fixture.drag.Entities().size() == 1);
    CHECK(fixture.drag.Entities()[0] == dragged);

    // And that other entity is mid-edit itself, with a gesture somebody else opened.
    fixture.history.RecordBefore(other, fixture.kTransformId, "Edit Transform", other);
    fixture.Nudge(other, 2.f);

    fixture.ReleasedFrame();

    // One transaction, and it is the drag's. The other entity's gesture is still
    // open — the release edge had no business closing it.
    REQUIRE(fixture.history.UndoDepth() == 1);
    (void)fixture.history.Undo();
    CHECK(fixture.X(dragged) == doctest::Approx(0.f));
    CHECK(fixture.X(other) == doctest::Approx(7.f));

    // Proof it was still open rather than quietly committed-and-dropped: the sweep
    // that owns it closes it, with the pre-edit value intact.
    fixture.history.EndFrameSweep(false);
    REQUIRE(fixture.history.UndoDepth() == 1);
    (void)fixture.history.Undo();
    CHECK(fixture.X(other) == doctest::Approx(5.f));
}

TEST_CASE("GizmoDrag: deleting the dragged entity ends the drag without committing it")
{
    Fixture fixture;
    const Entity dragged = fixture.Spawn(0.f);
    const Entity next    = fixture.Spawn(9.f);

    for (int32_t frame = 0; frame < 10; ++frame)
        fixture.HeldFrame(dragged, {}, 0.1f);

    // The author deletes what they were dragging. This is the path the old code
    // could not reach its own release edge through: the draw function returns at
    // `!IsAlive(_selectedEntity)`, long before the commit at the bottom.
    fixture.scene.Destroy(dragged);
    fixture.scene.FlushDestroyed();

    fixture.ReleasedFrame();

    // Nothing recorded. The component is gone, so committing would read the absence
    // as a *removal* and push a transaction whose undo re-adds a Transform to a dead
    // handle. EditHistory's own sweep drops such gestures; so does this.
    CHECK(fixture.history.UndoDepth() == 0);
    // And the edge is spent, which is the half that used to go wrong.
    CHECK_FALSE(fixture.drag.IsOpen());

    // The next frame: the author selects something else, and the inspector opens a
    // gesture on it. The stale edge used to fire right here, against this entity.
    fixture.history.RecordBefore(next, fixture.kTransformId, "Edit Transform", next);
    fixture.ReleasedFrame();
    fixture.Nudge(next, 3.f);
    fixture.history.EndFrameSweep(false);

    // Its gesture survived the release intact — one transaction, holding the whole
    // edit. A stale edge would have closed it while before == after, dropping the
    // gesture, and this edit would have gone unrecorded.
    REQUIRE(fixture.history.UndoDepth() == 1);
    (void)fixture.history.Undo();
    CHECK(fixture.X(next) == doctest::Approx(9.f));
}

TEST_CASE("GizmoDrag: a click that moves nothing records nothing")
{
    Fixture fixture;
    const Entity handle = fixture.Spawn(2.f);

    // Grabbing a handle and letting go. The drag opens — it cannot know yet whether
    // a drag is coming — and closes with before == after.
    for (int32_t frame = 0; frame < 5; ++frame)
        fixture.HeldFrame(handle, {}, 0.f);
    fixture.ReleasedFrame();

    CHECK(fixture.history.UndoDepth() == 0);
    CHECK_FALSE(fixture.history.CanUndo());
}

TEST_CASE("GizmoDrag: releasing with no drag open leaves other gestures alone")
{
    Fixture fixture;
    const Entity edited = fixture.Spawn(1.f);

    // The release runs on every frame of the editor's life, and almost none of them
    // are a drag. Meanwhile the inspector is scrubbing a Transform field.
    fixture.history.RecordBefore(edited, fixture.kTransformId, "Edit Transform", edited);
    for (int32_t frame = 0; frame < 100; ++frame)
    {
        fixture.Nudge(edited, 0.01f);
        fixture.ReleasedFrame();
    }

    CHECK(fixture.history.UndoDepth() == 0);
    CHECK_FALSE(fixture.drag.IsOpen());

    // Still one gesture, still holding the value from before the scrub started.
    fixture.history.EndFrameSweep(false);
    REQUIRE(fixture.history.UndoDepth() == 1);
    (void)fixture.history.Undo();
    CHECK(fixture.X(edited) == doctest::Approx(1.f));
}
