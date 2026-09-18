/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintAuthoring.cpp
/// @brief Making a blueprint out of a selection, and placing copies of it.
///
/// "Create blueprint from selection" is not a new mechanism — it is *save the
/// selection as a file, and replace it with an instance of that file*
/// So the property that matters is that
/// the swap is invisible: the entities that come back stand exactly where the
/// originals did, and a second copy placed elsewhere carries the same shape.
///
/// The two things that would silently break it are the ones pinned below — the
/// members must be written around the file's own origin (or every copy appears at
/// the first one's position), and a placement made by an author must be marked as
/// level content (or the first save drops it).

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

#include "LogCapture.hpp"

using namespace Assisi;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bpa_" + name);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

ECS::Transform At(float x, float y, float z)
{
    ECS::Transform transform;
    transform.position = {x, y, z};
    return transform;
}

/// A body at (10,0,0) with a lid parented to it one unit up — the shape a user
/// would select and turn into a blueprint.
struct Selection
{
    ECS::Entity body;
    ECS::Entity lid;
    std::vector<ECS::Entity> all;
};

Selection BuildSelection(ECS::Scene &scene)
{
    Selection out;
    out.body = scene.Create();
    REQUIRE(scene.Add(out.body, At(10.f, 0.f, 0.f)) != nullptr);
    REQUIRE(scene.Add(out.body, Runtime::Name{Core::EntityName{"body"}}) != nullptr);

    out.lid = scene.Create();
    REQUIRE(scene.Add(out.lid, At(0.f, 1.f, 0.f)) != nullptr);
    REQUIRE(scene.Add(out.lid, Runtime::Name{Core::EntityName{"lid"}}) != nullptr);
    REQUIRE(scene.Add(out.lid, Runtime::Parent{.parent = out.body}) != nullptr);

    out.all = {out.body, out.lid};
    return out;
}

} // namespace

TEST_CASE("Authoring: a selection saved as a blueprint places back exactly where it was")
{
    const std::filesystem::path root = FreshRoot("roundtrip");

    ECS::Scene scene;
    const Selection selection = BuildSelection(scene);

    const ECS::Transform origin = *scene.Get<ECS::Transform>(selection.body);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp", origin));

    // The file is authored around its own origin, so the body sits at zero rather
    // than at (10,0,0). Without this every copy would appear wherever the original
    // happened to be standing, and the second one would be ten units off.
    const Runtime::BlueprintResult loaded = Runtime::GetBlueprintDefinition("crate.abp");
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const Runtime::BlueprintDefinition> &definition = *loaded;
    REQUIRE(definition->members.size() == 2);
    CHECK(definition->members[0].name == "body");
    CHECK(Runtime::TransformFromJson(definition->members[0].components.at("Transform")).position.x ==
          doctest::Approx(0.f));

    // The lid is parented, so it reaches the origin through its parent and keeps
    // its own offset — dividing it too would divide twice.
    CHECK(definition->members[1].parented);
    CHECK(Runtime::TransformFromJson(definition->members[1].components.at("Transform")).position.y ==
          doctest::Approx(1.f));

    // Placed back at the same origin, the copy stands where the original did.
    InstanceTable table;
    const Runtime::LevelInstance entry{.name      = "crate_1",
                                       .source    = "crate.abp",
                                       .transform = origin,
                                       .overrides = nlohmann::json::object(),
                                       .removed   = {}};
    const auto placed = SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/ true);
    REQUIRE(placed.has_value());
    REQUIRE(placed->members.size() == 2);

    const ECS::Transform *body = scene.Get<ECS::Transform>(placed->members[0]);
    REQUIRE(body != nullptr);
    CHECK(body->position.x == doctest::Approx(10.f));

    // And the wiring survived: the copy's lid parents to the copy's body, not to
    // the original's.
    const Runtime::Parent *parent = scene.Get<Runtime::Parent>(placed->members[1]);
    REQUIRE(parent != nullptr);
    CHECK(parent->parent == placed->members[0]);
    CHECK(parent->parent != selection.body);
}

TEST_CASE("Authoring: a second copy stands where it was put, not where the first is")
{
    const std::filesystem::path root = FreshRoot("second");

    ECS::Scene scene;
    const Selection selection = BuildSelection(scene);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    InstanceTable table;
    const auto place = [&](const char *name, float x)
                       {
                           const Runtime::LevelInstance entry{.name      = name,
                                                              .source    = "crate.abp",
                                                              .transform = At(x, 0.f, 0.f),
                                                              .overrides = nlohmann::json::object(),
                                                              .removed   = {}};
                           return SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/ true);
                       };

    const auto first  = place("crate_1", 3.f);
    const auto second = place("crate_2", -7.f);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK(scene.Get<ECS::Transform>(first->members[0])->position.x == doctest::Approx(3.f));
    CHECK(scene.Get<ECS::Transform>(second->members[0])->position.x == doctest::Approx(-7.f));
}

TEST_CASE("Authoring: an instance an author placed is written back; a runtime spawn is not")
{
    const std::filesystem::path root = FreshRoot("authored");

    ECS::Scene scene;
    const Selection selection = BuildSelection(scene);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    ECS::Scene level;
    InstanceTable table;

    const Runtime::LevelInstance entry{.name      = "crate_1",
                                       .source    = "crate.abp",
                                       .transform = At(4.f, 0.f, 0.f),
                                       .overrides = nlohmann::json::object(),
                                       .removed   = {}};
    REQUIRE(SceneSerializer::PlaceInstance(level, table, entry, /*authored=*/ true).has_value());

    // A runtime spawn exists because something in the game asked for it; writing it
    // into the file would make it authored the next time the level loads.
    REQUIRE(SceneSerializer::ExpandInstance(level, table, "crate.abp", At(0.f, 0.f, 0.f)).has_value());

    const nlohmann::json saved = SceneSerializer::Save(level, {}, &table);
    REQUIRE(saved.contains("instances"));
    REQUIRE(saved.at("instances").size() == 1);
    CHECK(saved.at("instances")[0].at("name").get<std::string>() == "crate_1");
}

TEST_CASE("Authoring: a save renumbers the rows it wrote to match the file it wrote them into")
{
    const std::filesystem::path root = FreshRoot("renumber");

    ECS::Scene scene;
    const Selection selection = BuildSelection(scene);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    ECS::Scene level;
    InstanceTable table;

    // An editor placement is authored the moment it is made, but carries no file
    // position: nothing has written it into one yet.
    const auto place = [&](const char *name, float x)
                       {
                           const Runtime::LevelInstance entry{.name      = name,
                                                              .source    = "crate.abp",
                                                              .transform = At(x, 0.f, 0.f),
                                                              .overrides = nlohmann::json::object(),
                                                              .removed   = {}};
                           const auto placed = SceneSerializer::PlaceInstance(level, table, entry, /*authored=*/ true);
                           REQUIRE(placed.has_value());
                           return placed->instanceId;
                       };

    const ECS::InstanceId first  = place("crate_1", 4.f);
    const ECS::InstanceId second = place("crate_2", 8.f);
    const ECS::InstanceId third  = place("crate_3", 12.f);

    REQUIRE(table.Find(first)->levelInstanceIndex == -1);
    REQUIRE(table.Find(second)->levelInstanceIndex == -1);

    // The middle one goes, so the survivors' positions in the file are not the
    // positions they would have had.
    table.Remove(second);

    const nlohmann::json saved = SceneSerializer::Save(level, {}, &table);
    REQUIRE(saved.at("instances").size() == 2);
    CHECK(saved.at("instances")[0].at("name").get<std::string>() == "crate_1");
    CHECK(saved.at("instances")[1].at("name").get<std::string>() == "crate_3");

    // What the rows say must be what the file says: a stale index is a row claiming
    // an entry that belongs to another instance, which is what the shared-baseline
    // join reads to pair the two sides up.
    CHECK(table.Find(first)->levelInstanceIndex == 0);
    CHECK(table.Find(third)->levelInstanceIndex == 1);
}

TEST_CASE("Authoring: a selection containing a blueprint member is refused")
{
    const std::filesystem::path root = FreshRoot("nesting");

    ECS::Scene scene;
    const Selection selection = BuildSelection(scene);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    InstanceTable table;
    const Runtime::LevelInstance entry{.name      = "crate_1",
                                       .source    = "crate.abp",
                                       .transform = {},
                                       .overrides = nlohmann::json::object(),
                                       .removed   = {}};
    const auto placed = SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/ true);
    REQUIRE(placed.has_value());

    // Nesting is an `instances` entry, not copied entities. Copying them would bake
    // the inner blueprint into the new file and stop a fix to it from reaching this
    // one — the exact thing the format exists to prevent.
    const std::vector<ECS::Entity> withMember{placed->members[0]};
    CHECK_FALSE(SceneSerializer::SaveEntitiesToFile(scene, withMember, root / "nested.abp", {}));
    CHECK_FALSE(std::filesystem::exists(root / "nested.abp"));
}

TEST_CASE("Authoring: children saved without their parent stand where they stood")
{
    const std::filesystem::path root = FreshRoot("parented");

    ECS::Scene scene;
    const ECS::Entity rig = scene.Create();
    REQUIRE(scene.Add(rig, At(100.f, 0.f, 0.f)) != nullptr);
    REQUIRE(scene.Add(rig, Runtime::Name{Core::EntityName{"rig"}}) != nullptr);

    // The crate hangs off the rig, and the rig is not being captured — saving the
    // parts of a rig without the rig is a thing an author means to do, so it is
    // supported rather than refused.
    const Selection selection = BuildSelection(scene);
    REQUIRE(scene.Add(selection.body, Runtime::Parent{.parent = rig}) != nullptr);

    // The body's own Transform reads (10,0,0), but that is an offset from the rig:
    // it *stands* at (110,0,0). The origin has to be where it stands, or the
    // instance is placed into a space that is not coming with it.
    const ECS::Transform origin = Runtime::AuthoringOriginFor(scene, selection.all);
    CHECK(origin.position.x == doctest::Approx(110.f));

    const Tests::LogCapture log;
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp", origin));

    // The parent is not in the file, so it nulls — and the body is a root of this
    // file. Writing its raw (10,0,0) would leave it measured from a rig the file
    // does not have, and the copy would come back a hundred units from where the
    // author was looking (round-7 S16). Written around the origin, it is at zero.
    const Runtime::BlueprintResult loaded = Runtime::GetBlueprintDefinition("crate.abp");
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const Runtime::BlueprintDefinition> &definition = *loaded;
    REQUIRE(definition->members.size() == 2);
    CHECK(definition->members[0].components.at("Parent").at("parent").is_null());
    CHECK_FALSE(definition->members[0].parented); // a cut parent makes it a root
    CHECK(Runtime::TransformFromJson(definition->members[0].components.at("Transform")).position.x ==
          doctest::Approx(0.f));
    CHECK(log.Mentions("rig"));

    // Placed back at that origin, the swap is invisible: the body stands exactly
    // where it stood, and the lid keeps its offset under it.
    InstanceTable table;
    const Runtime::LevelInstance entry{.name      = "crate_1",
                                       .source    = "crate.abp",
                                       .transform = origin,
                                       .overrides = nlohmann::json::object(),
                                       .removed   = {}};
    const auto placed = SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/ true);
    REQUIRE(placed.has_value());
    REQUIRE(placed->members.size() == 2);

    const ECS::Transform *body = scene.Get<ECS::Transform>(placed->members[0]);
    REQUIRE(body != nullptr);
    CHECK(body->position.x == doctest::Approx(110.f));
}

TEST_CASE("Authoring: the origin comes from the set being written, not from beside it")
{
    // Round-7 S16's other half. An anchor taken from the selection rather than from
    // the captured set can be an entity that was skipped on the way in — a dead or
    // uneditable one — and then every member is written around a pose no member has
    // and the whole copy stands off by the difference. Taking the set rather than an
    // entity is what makes that unsayable, so the contract worth pinning is that the
    // front of the set is the anchor.
    ECS::Scene scene;

    ECS::Transform pose = At(4.f, 0.f, 0.f);
    pose.scale          = {0.6f, 0.6f, 0.6f};
    const ECS::Entity first = scene.Create();
    REQUIRE(scene.Add(first, pose) != nullptr);

    const ECS::Entity second = scene.Create();
    REQUIRE(scene.Add(second, At(-9.f, 0.f, 0.f)) != nullptr);

    const ECS::Transform origin = Runtime::AuthoringOriginFor(scene, std::vector<ECS::Entity>{first, second});
    CHECK(origin.position.x == doctest::Approx(4.f));
    // Same rule as the single-entity form: placement carries where and which way,
    // never how big.
    CHECK(origin.scale.x == doctest::Approx(1.f));

    // Order is the set's, so the anchor does not drift with click order.
    const ECS::Transform reversed =
        Runtime::AuthoringOriginFor(scene, std::vector<ECS::Entity>{second, first});
    CHECK(reversed.position.x == doctest::Approx(-9.f));

    // An entity with no Transform anchors at the identity rather than at whatever
    // the next one happens to have: the file's origin is the front of the set.
    const ECS::Entity poseless = scene.Create();
    const ECS::Transform none = Runtime::AuthoringOriginFor(scene, std::vector<ECS::Entity>{poseless, first});
    CHECK(none.position.x == doctest::Approx(0.f));
}

TEST_CASE("Authoring: a scaled selection stays scaled in every copy")
{
    const std::filesystem::path root = FreshRoot("scale");

    ECS::Scene scene;
    ECS::Entity cube = scene.Create();
    ECS::Transform pose = At(10.f, 0.f, 0.f);
    pose.scale          = {0.6f, 0.6f, 0.6f};
    REQUIRE(scene.Add(cube, pose) != nullptr);
    REQUIRE(scene.Add(cube, Runtime::Name{Core::EntityName{"cube"}}) != nullptr);

    // The origin an author's selection is written around carries no scale, so the
    // file records the cube at the size it was drawn at. Divide the scale out into
    // the placement instead and the file holds a unit cube: the copy that replaced
    // the original still looks right (its placement carries the scale back), and
    // every fresh instance comes back full size.
    const ECS::Transform origin = Runtime::AuthoringOrigin(pose);
    CHECK(origin.position.x == doctest::Approx(10.f));
    CHECK(origin.scale.x == doctest::Approx(1.f));

    const std::vector<ECS::Entity> selection{cube};
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection, root / "small_crate.abp", origin));

    const Runtime::BlueprintResult loaded = Runtime::GetBlueprintDefinition("small_crate.abp");
    REQUIRE(loaded.has_value());
    const std::shared_ptr<const Runtime::BlueprintDefinition> &definition = *loaded;
    REQUIRE(definition->members.size() == 1);
    CHECK(Runtime::TransformFromJson(definition->members[0].components.at("Transform")).scale.x ==
          doctest::Approx(0.6f));

    // A copy placed somewhere else entirely is still the size it was saved at.
    InstanceTable table;
    const Runtime::LevelInstance entry{.name      = "small_crate_1",
                                       .source    = "small_crate.abp",
                                       .transform = At(-4.f, 0.f, 0.f),
                                       .overrides = nlohmann::json::object(),
                                       .removed   = {}};
    const auto placed = SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/ true);
    REQUIRE(placed.has_value());

    const ECS::Transform *copy = scene.Get<ECS::Transform>(placed->members[0]);
    REQUIRE(copy != nullptr);
    CHECK(copy->position.x == doctest::Approx(-4.f));
    CHECK(copy->scale.x == doctest::Approx(0.6f));

    // …and scaling the *instance* still multiplies on top, so the two are not the
    // same knob — one is what the thing is, the other is what this copy of it is.
    ECS::Transform doubled = At(0.f, 0.f, 0.f);
    doubled.scale          = {2.f, 2.f, 2.f};
    const Runtime::LevelInstance bigEntry{.name      = "small_crate_2",
                                          .source    = "small_crate.abp",
                                          .transform = doubled,
                                          .overrides = nlohmann::json::object(),
                                          .removed   = {}};
    const auto big = SceneSerializer::PlaceInstance(scene, table, bigEntry, /*authored=*/ true);
    REQUIRE(big.has_value());
    CHECK(scene.Get<ECS::Transform>(big->members[0])->scale.x == doctest::Approx(1.2f));
}

TEST_CASE("Authoring: a reference leaving the selection is nulled, not dangled")
{
    const std::filesystem::path root = FreshRoot("outref");

    ECS::Scene scene;
    const Selection selection = BuildSelection(scene);
    const ECS::Entity outsider  = scene.Create();
    REQUIRE(scene.Add(outsider, At(0.f, 0.f, 0.f)) != nullptr);
    REQUIRE(scene.Add(outsider, Runtime::Name{Core::EntityName{"marker"}}) != nullptr);

    // The body now points at something the blueprint will not contain.
    scene.GetMut<Runtime::Parent>(selection.lid)->parent = outsider;

    const Tests::LogCapture log;

    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    // The file cannot name what it does not contain, so the reference is null —
    // which loads, rather than refusing on an unknown name.
    const Runtime::BlueprintResult loaded = Runtime::GetBlueprintDefinition("crate.abp");
    REQUIRE(loaded.has_value());
    CHECK((*loaded)->members[1].components.at("Parent").at("parent").is_null());

    // And the author is told, which is the half the null cannot carry: "create
    // blueprint from selection" is a gesture people make on a subset of a wired-up
    // level, and every wire leaving that subset is cut by it. Named down to the
    // field, because "some reference was dropped" does not tell anyone what to
    // re-wire.
    CHECK(log.Mentions("Parent::parent on 'lid'"));
}
