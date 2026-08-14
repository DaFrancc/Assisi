/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestInstanceGesture.cpp
/// @brief One placement gesture is one undo entry, however many frames it took.
///
/// The failure this exists to prevent has a name: **per-frame history**. An
/// instance's placement is written by two sites — the gizmo handles and the
/// Inspector's Placement fields — and each of them used to decide on its own
/// when the gesture was over. The gizmo drew first in the frame, so an Inspector
/// scrub opened a gesture the gizmo closed at the top of the next frame, over
/// and over: sixty frames of dragging one field, sixty transactions, and a
/// `kMaxDepth`-deep stack of noise where the author's real edits used to be.
/// Ctrl-Z then walked the drag back one frame at a time and never reached the
/// value the author started from (round-7 B19).
///
/// So the assertion that matters is not "a transaction is pushed" but "*one* is,
/// and undoing it lands back where the drag began" — which is the only thing the
/// author was ever asking for.

#include <doctest/doctest.h>

#include <ostream>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Editor/EditHistory.hpp>
#include <Assisi/Editor/InstanceGesture.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Editor::EditHistory;
using Assisi::Editor::InstanceGesture;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;
using Assisi::Runtime::Transform;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_gest_" + name);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

/// Its own function so the stream is closed — and therefore flushed — before the
/// caller reads the file back. A few hundred bytes never leave the buffer on
/// their own.
void Write(const std::filesystem::path &path, const nlohmann::json &doc)
{
    std::ofstream out(path, std::ios::binary);
    out << doc.dump(2);
    REQUIRE(out.good());
}

/// cart.abp: a body at the origin and a crate standing beside it. Both carry a
/// Transform and neither is parented, so a placement move reaches both — which is
/// what makes the committed transaction hold more than the record alone.
nlohmann::json CartFile()
{
    const auto placed = [](float x) {
                            return nlohmann::json{{"Transform",
                                {{"position", {x, 0.0, 0.0}},
                                    {"rotation", {1.0, 0.0, 0.0, 0.0}},
                                    {"scale", {1.0, 1.0, 1.0}}}}};
                        };
    return {{"version", 2},
        {"entities", nlohmann::json::array({{{"name", "body"}, {"components", placed(0.f)}},
                                               {{"name", "crate"}, {"components", placed(2.f)}}})}};
}

/// What ApplyInstancePlacement does to the scene, minus the physics sync: shift
/// the record and carry every member the placement reaches by the same delta.
/// The gesture under test does not move anything itself — it only decides when
/// what moved becomes an undo entry — so the test moves the scene the same way
/// the editor does and watches what the gesture makes of it.
void NudgeInstance(ECS::Scene &scene, InstanceTable &table, ECS::InstanceId id, float dx)
{
    const Runtime::BlueprintInstance *row = table.Find(id);
    REQUIRE(row != nullptr);

    for (const ECS::Entity member : Runtime::MembersOf(scene, id))
    {
        if (scene.Has<Runtime::Parent>(member))
            continue;
        if (Transform *transform = scene.GetMut<Transform>(member))
            transform->position.x += dx;
    }

    Runtime::BlueprintInstance moved = *row;
    moved.transform.position.x += dx;
    table.RestoreAt(id, std::move(moved));
}

float BodyX(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId id)
{
    const ECS::Entity body = Runtime::FindMember(scene, table, id, "body");
    REQUIRE(body != ECS::NullEntity);
    return scene.Get<Transform>(body)->position.x;
}

/// A world the editor would be drawing: the file on disk, one instance of it, and
/// a history bound to both.
struct Fixture
{
    explicit Fixture(const std::string &name) : root(FreshRoot(name))
    {
        Write(root / "cart.abp", CartFile());

        const auto placed =
            SceneSerializer::ExpandInstance(scene, table, "cart.abp", {});
        REQUIRE(placed.has_value());
        id = *placed;

        history.emplace(scene, EditHistory::RebindHook{}, &table);
    }

    /// One frame in which an edit site still has the placement: it raises the
    /// hold, opens (or re-opens) the gesture, writes, and then the end-of-frame
    /// sweep runs — exactly the order OnImGui uses.
    void HeldFrame(float dx)
    {
        gesture.Hold();
        gesture.Begin(scene, table, &*history, id);
        NudgeInstance(scene, table, id, dx);
        gesture.EndFrame(scene, table, &*history, "Move Instance");
    }

    /// A frame in which nobody held it: the sweep runs alone, as it does on every
    /// frame of the editor's life.
    void ReleasedFrame() { gesture.EndFrame(scene, table, &*history, "Move Instance"); }

    std::filesystem::path root;
    ECS::Scene scene;
    InstanceTable table;
    ECS::InstanceId id;
    std::optional<EditHistory> history;
    InstanceGesture gesture;
};

} // namespace

TEST_CASE("InstanceGesture: a scrub held across many frames is one undo entry")
{
    Fixture fixture("scrub");
    const float startX = BodyX(fixture.scene, fixture.table, fixture.id);

    // Sixty frames of holding one Placement field and dragging it right — a bit
    // under a second at 60 Hz, which is a short drag, not a long one.
    constexpr int32_t kFrames = 60;
    for (int32_t frame = 0; frame < kFrames; ++frame)
        fixture.HeldFrame(0.1f);

    // Still nothing recorded: the drag has not finished, so there is no completed
    // edit to record. Anything here is per-frame noise by definition.
    CHECK(fixture.history->UndoDepth() == 0);
    CHECK(fixture.gesture.IsOpen());

    fixture.ReleasedFrame();

    CHECK_FALSE(fixture.gesture.IsOpen());
    CHECK(fixture.history->UndoDepth() == 1);

    // And it is the *whole* drag. This is the half that per-frame transactions got
    // wrong in a way a depth count alone would not catch: sixty entries would also
    // leave the scene at the right place, and Ctrl-Z would still walk back 0.1 at a
    // time and land nowhere the author recognises.
    const float endX = BodyX(fixture.scene, fixture.table, fixture.id);
    CHECK(endX == doctest::Approx(startX + 6.f));

    REQUIRE(fixture.history->CanUndo());
    (void)fixture.history->Undo();
    CHECK(BodyX(fixture.scene, fixture.table, fixture.id) == doctest::Approx(startX));
    CHECK(fixture.table.Find(fixture.id)->transform.position.x == doctest::Approx(0.f));
    CHECK(fixture.history->UndoDepth() == 0);
}

TEST_CASE("InstanceGesture: two separate drags are two undo entries")
{
    Fixture fixture("twice");

    for (int32_t frame = 0; frame < 10; ++frame)
        fixture.HeldFrame(0.5f);
    fixture.ReleasedFrame();

    // A gap of idle frames between them, as there always is: the author let go,
    // looked at it, and grabbed the field again.
    fixture.ReleasedFrame();
    fixture.ReleasedFrame();
    CHECK(fixture.history->UndoDepth() == 1);

    for (int32_t frame = 0; frame < 10; ++frame)
        fixture.HeldFrame(0.5f);
    fixture.ReleasedFrame();

    CHECK(fixture.history->UndoDepth() == 2);

    // One Ctrl-Z takes back the second drag and only the second.
    (void)fixture.history->Undo();
    CHECK(fixture.table.Find(fixture.id)->transform.position.x == doctest::Approx(5.f));
}

TEST_CASE("InstanceGesture: a click that moves nothing records nothing")
{
    Fixture fixture("noop");

    // Grabbing the handle and letting go without moving. The gesture opens, because
    // the site cannot know yet whether a drag is coming, and closes with nothing.
    for (int32_t frame = 0; frame < 5; ++frame)
        fixture.HeldFrame(0.f);
    fixture.ReleasedFrame();

    CHECK(fixture.history->UndoDepth() == 0);
    CHECK_FALSE(fixture.history->CanUndo());
}

TEST_CASE("InstanceGesture: an all-parented instance still records its move")
{
    Fixture fixture("parented");

    // Every member attached to something outside the instance, which is what a
    // level-side Parent override does (round-7 B3's territory). The placement
    // reaches none of them now — a parented member rides along through its parent,
    // so carrying it here would apply the move twice — and the gesture is left with
    // no member pose to compare. The *record* still moved, and the record is what
    // InstancesForSave writes.
    const ECS::Entity anchor = fixture.scene.Create();
    (void)fixture.scene.Add<Transform>(anchor);
    for (const ECS::Entity member : Runtime::MembersOf(fixture.scene, fixture.id))
        (void)fixture.scene.Add<Runtime::Parent>(member, Runtime::Parent{anchor});

    for (int32_t frame = 0; frame < 10; ++frame)
        fixture.HeldFrame(0.5f);
    fixture.ReleasedFrame();

    // The three states have to agree. Before this was fixed they did not: the row
    // was at 5, the save would have written 5, and the history was empty — so the
    // move was on disk, not undoable, and never marked the scene dirty.
    CHECK(fixture.history->UndoDepth() == 1);
    CHECK(fixture.table.Find(fixture.id)->transform.position.x == doctest::Approx(5.f));

    REQUIRE(fixture.history->CanUndo());
    (void)fixture.history->Undo();
    CHECK(fixture.table.Find(fixture.id)->transform.position.x == doctest::Approx(0.f));
}

TEST_CASE("InstanceGesture: an instance whose members are all gone still records its move")
{
    Fixture fixture("nomembers");

    // Deleting every member does not remove the row — InstanceTable::Remove is a
    // separate call the entity delete never makes — so the instance is still
    // selectable and still saved, with nothing left for the placement to carry.
    for (const ECS::Entity member : Runtime::MembersOf(fixture.scene, fixture.id))
        fixture.scene.Destroy(member);
    fixture.scene.FlushDestroyed();
    REQUIRE(Runtime::MembersOf(fixture.scene, fixture.id).empty());

    for (int32_t frame = 0; frame < 10; ++frame)
        fixture.HeldFrame(0.5f);
    fixture.ReleasedFrame();

    CHECK(fixture.history->UndoDepth() == 1);
    CHECK(fixture.table.Find(fixture.id)->transform.position.x == doctest::Approx(5.f));

    REQUIRE(fixture.history->CanUndo());
    (void)fixture.history->Undo();
    CHECK(fixture.table.Find(fixture.id)->transform.position.x == doctest::Approx(0.f));
}

TEST_CASE("InstanceGesture: an idle sweep on a closed gesture does nothing")
{
    Fixture fixture("idle");

    // The overwhelmingly common case — the sweep runs every frame of the editor's
    // life, and almost none of those frames are a drag.
    for (int32_t frame = 0; frame < 100; ++frame)
        fixture.ReleasedFrame();

    CHECK(fixture.history->UndoDepth() == 0);
    CHECK_FALSE(fixture.gesture.IsOpen());
}
