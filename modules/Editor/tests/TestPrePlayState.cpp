/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPrePlayState.cpp
/// @brief Stop puts the edited world's instance table back exactly as Run found
/// it — round-7 finding B4.
///
/// The failure this exists to prevent edits a file nobody asked it to edit.
/// Joining a session loads the *host's* level into the edited world, which
/// clears the instance table and refills it with the host's rows, every one of
/// them flagged `authored`. Stop used to restore the entities, the level path
/// and the system list — and not the table. So you came back to your own level
/// holding somebody else's instances, with no dirty marker and nothing on screen
/// to say so, and the next Save wrote them into your file over your own.
///
/// What is covered here is the restore itself: capture, let a session overwrite
/// the table however it likes, restore, and check that what comes back is the
/// author's table down to the id allocator. What is *not* covered is StopPlay
/// actually calling it — that path needs a live EditorApp (ImGui, GLFW, Jolt)
/// and there is no headless harness for it. Keeping the capture and the restore
/// to one call each is the structural half of that: B4 was a forgotten field in
/// a per-field capture, and a field cannot be forgotten from a struct that is
/// assigned whole.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <Assisi/Editor/PrePlayState.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

using Assisi::Editor::CapturePrePlayState;
using Assisi::Editor::PrePlayState;
using Assisi::Editor::RestorePrePlayState;
using Assisi::Runtime::BlueprintInstance;
using Assisi::Runtime::InstancesForSave;
using Assisi::Runtime::InstanceTable;

namespace
{

/// A row as the editor records one the author placed: named, from a file, and
/// level content.
BlueprintInstance Authored(std::string name, std::string source)
{
    BlueprintInstance row;
    row.name     = std::move(name);
    row.source   = std::move(source);
    row.authored = true;
    return row;
}

/// A row as a runtime spawn leaves one: no name, and not level content.
BlueprintInstance Spawned(std::string source)
{
    BlueprintInstance row;
    row.source   = std::move(source);
    row.authored = false;
    return row;
}

} // namespace

TEST_CASE("Join then Stop leaves the level's own instances, not the host's")
{
    // The level in front of the author: two placed instances.
    InstanceTable       instances;
    const auto          mine1 = instances.Add(Authored("car_1", "blueprints/car.abp"));
    const auto          mine2 = instances.Add(Authored("crate_1", "blueprints/crate.abp"));
    std::string         levelPath{"levels/mine.alvl"};
    std::vector<std::string> systems{"Spin"};

    const PrePlayState captured = CapturePrePlayState(levelPath, systems, instances);

    // Join: LoadFromFile clears the table and fills it with the host's rows,
    // every one of them authored (they came from a level file), and retargets
    // the world at the host's level.
    instances.Clear();
    const auto theirs = instances.Add(Authored("tower_1", "blueprints/tower.abp"));
    instances.Add(Authored("tower_2", "blueprints/tower.abp"));
    levelPath = "levels/theirs.alvl";
    REQUIRE(instances.Size() == 2);

    const bool systemsChanged = RestorePrePlayState(captured, levelPath, systems, instances);

    CHECK(levelPath == "levels/mine.alvl");
    CHECK_FALSE(systemsChanged); // the join never touched this world's system list

    // The author's rows, at the ids their BlueprintMember tags still name.
    REQUIRE(instances.Size() == 2);
    REQUIRE(instances.Find(mine1) != nullptr);
    REQUIRE(instances.Find(mine2) != nullptr);
    CHECK(instances.Find(mine1)->name == "car_1");
    CHECK(instances.Find(mine2)->name == "crate_1");

    // …and nothing of the host's, however its ids happened to land. (Both tables
    // number from 1, so `theirs` collides with `mine1` by construction — the
    // check that matters is what the row *says*.)
    CHECK(instances.Find(theirs)->source != "blueprints/tower.abp");

    // The point of all of it: the next Save writes the author's level back.
    const auto forSave = InstancesForSave(instances);
    REQUIRE(forSave.size() == 2);
    CHECK(forSave[0].source == "blueprints/car.abp");
    CHECK(forSave[1].source == "blueprints/crate.abp");
}

TEST_CASE("A blueprint spawned during play does not survive Stop")
{
    // The lesser variant of B4, and it needs no networking: SpawnBlueprint
    // during play adds a row to the edited world's table, and a row that
    // outlives the session is a ghost the outliner shows with no live members.
    InstanceTable instances;
    const auto    authored = instances.Add(Authored("car_1", "blueprints/car.abp"));

    std::string              levelPath{"levels/mine.alvl"};
    std::vector<std::string> systems;
    const PrePlayState       captured = CapturePrePlayState(levelPath, systems, instances);

    const auto spawned = instances.Add(Spawned("blueprints/bullet.abp"));
    REQUIRE(instances.Size() == 2);

    RestorePrePlayState(captured, levelPath, systems, instances);

    CHECK(instances.Size() == 1);
    CHECK(instances.Find(authored) != nullptr);
    CHECK(instances.Find(spawned) == nullptr);
}

TEST_CASE("Stop puts the instance id allocator back, not just the rows")
{
    // Ids are what the undo history recorded against: a transaction that deleted
    // instance 3 restores it at 3 (InstanceTable::RestoreAt), so a table whose
    // allocator regressed hands 3 out again to something else and the undo lands
    // on a live row. Rebuilding the table row by row would do exactly that —
    // which is why the restore puts the whole table back rather than replaying
    // its contents.
    InstanceTable instances;
    instances.Add(Authored("car_1", "blueprints/car.abp"));
    instances.Add(Authored("car_2", "blueprints/car.abp"));
    const auto deleted = instances.Add(Authored("car_3", "blueprints/car.abp"));
    instances.Remove(deleted); // deleted in the editor, undoable — id 3 is spoken for

    std::string              levelPath{"levels/mine.alvl"};
    std::vector<std::string> systems;
    const PrePlayState       captured = CapturePrePlayState(levelPath, systems, instances);

    instances.Clear();
    instances.Add(Authored("tower_1", "blueprints/tower.abp"));

    RestorePrePlayState(captured, levelPath, systems, instances);

    const auto next = instances.Add(Authored("car_4", "blueprints/car.abp"));
    CHECK(next.value == 4u);
    CHECK(instances.Find(deleted) == nullptr); // still free for the undo to land on
}

TEST_CASE("A join that retargeted the world's systems asks for them back")
{
    // The system list is not an assignment — ApplySystems installs, and can
    // fail — so the restore reports rather than performs it. Both directions
    // matter: a session that never touched the list must not provoke a
    // reinstall, and one that did must.
    InstanceTable            instances;
    std::string              levelPath{"levels/mine.alvl"};
    std::vector<std::string> systems{"Spin", "Bob"};

    const PrePlayState captured = CapturePrePlayState(levelPath, systems, instances);

    std::vector<std::string> hostSystems{"Tower"};
    CHECK(RestorePrePlayState(captured, levelPath, hostSystems, instances));
    CHECK_FALSE(RestorePrePlayState(captured, levelPath, systems, instances));
}
