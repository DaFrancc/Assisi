/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintAuthoring.cpp
/// @brief Making a blueprint out of a selection, and placing copies of it.
///
/// "Create blueprint from selection" is not a new mechanism — it is *save the
/// selection as a file, and replace it with an instance of that file*
/// (docs/blueprint-system-concept.md §1). So the property that matters is that
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

using namespace Assisi;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;

namespace
{

std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bpa_" + name);
    std::error_code             ec;
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
    ECS::Entity              body;
    ECS::Entity              lid;
    std::vector<ECS::Entity> all;
};

Selection BuildSelection(ECS::Scene &scene)
{
    Selection out;
    out.body = scene.Create();
    REQUIRE(scene.Add(out.body, At(10.f, 0.f, 0.f)) != nullptr);
    REQUIRE(scene.Add(out.body, Runtime::Name{Core::ShortString{"body"}}) != nullptr);

    out.lid = scene.Create();
    REQUIRE(scene.Add(out.lid, At(0.f, 1.f, 0.f)) != nullptr);
    REQUIRE(scene.Add(out.lid, Runtime::Name{Core::ShortString{"lid"}}) != nullptr);
    REQUIRE(scene.Add(out.lid, Runtime::Parent{.parent = out.body}) != nullptr);

    out.all = {out.body, out.lid};
    return out;
}

} // namespace

TEST_CASE("Authoring: a selection saved as a blueprint places back exactly where it was")
{
    const std::filesystem::path root = FreshRoot("roundtrip");

    ECS::Scene      scene;
    const Selection selection = BuildSelection(scene);

    const ECS::Transform origin = *scene.Get<ECS::Transform>(selection.body);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp", origin));

    // The file is authored around its own origin, so the body sits at zero rather
    // than at (10,0,0). Without this every copy would appear wherever the original
    // happened to be standing, and the second one would be ten units off.
    const Runtime::BlueprintDefinition *definition = Runtime::GetBlueprintDefinition("crate.abp");
    REQUIRE(definition != nullptr);
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
    InstanceTable                        table;
    const Runtime::LevelInstance         entry{.name      = "crate_1",
                                               .source    = "crate.abp",
                                               .transform = origin,
                                               .overrides = nlohmann::json::object(),
                                               .removed   = {}};
    const auto placed = SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/true);
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

    ECS::Scene      scene;
    const Selection selection = BuildSelection(scene);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    InstanceTable table;
    const auto    place = [&](const char *name, float x)
    {
        const Runtime::LevelInstance entry{.name      = name,
                                           .source    = "crate.abp",
                                           .transform = At(x, 0.f, 0.f),
                                           .overrides = nlohmann::json::object(),
                                           .removed   = {}};
        return SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/true);
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

    ECS::Scene      scene;
    const Selection selection = BuildSelection(scene);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    ECS::Scene    level;
    InstanceTable table;

    const Runtime::LevelInstance entry{.name      = "crate_1",
                                       .source    = "crate.abp",
                                       .transform = At(4.f, 0.f, 0.f),
                                       .overrides = nlohmann::json::object(),
                                       .removed   = {}};
    REQUIRE(SceneSerializer::PlaceInstance(level, table, entry, /*authored=*/true).has_value());

    // A runtime spawn exists because something in the game asked for it; writing it
    // into the file would make it authored the next time the level loads.
    REQUIRE(SceneSerializer::ExpandInstance(level, table, "crate.abp", At(0.f, 0.f, 0.f)).has_value());

    const nlohmann::json saved = SceneSerializer::Save(level, {}, &table);
    REQUIRE(saved.contains("instances"));
    REQUIRE(saved.at("instances").size() == 1);
    CHECK(saved.at("instances")[0].at("name").get<std::string>() == "crate_1");
}

TEST_CASE("Authoring: a selection containing a blueprint member is refused")
{
    const std::filesystem::path root = FreshRoot("nesting");

    ECS::Scene      scene;
    const Selection selection = BuildSelection(scene);
    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    InstanceTable                table;
    const Runtime::LevelInstance entry{.name      = "crate_1",
                                       .source    = "crate.abp",
                                       .transform = {},
                                       .overrides = nlohmann::json::object(),
                                       .removed   = {}};
    const auto placed = SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/true);
    REQUIRE(placed.has_value());

    // Nesting is an `instances` entry, not copied entities. Copying them would bake
    // the inner blueprint into the new file and stop a fix to it from reaching this
    // one — the exact thing the format exists to prevent.
    const std::vector<ECS::Entity> withMember{placed->members[0]};
    CHECK_FALSE(SceneSerializer::SaveEntitiesToFile(scene, withMember, root / "nested.abp", {}));
    CHECK_FALSE(std::filesystem::exists(root / "nested.abp"));
}

TEST_CASE("Authoring: a reference leaving the selection is nulled, not dangled")
{
    const std::filesystem::path root = FreshRoot("outref");

    ECS::Scene        scene;
    const Selection   selection = BuildSelection(scene);
    const ECS::Entity outsider  = scene.Create();
    REQUIRE(scene.Add(outsider, At(0.f, 0.f, 0.f)) != nullptr);
    REQUIRE(scene.Add(outsider, Runtime::Name{Core::ShortString{"marker"}}) != nullptr);

    // The body now points at something the blueprint will not contain.
    scene.GetMut<Runtime::Parent>(selection.lid)->parent = outsider;

    REQUIRE(SceneSerializer::SaveEntitiesToFile(scene, selection.all, root / "crate.abp",
                                                *scene.Get<ECS::Transform>(selection.body)));

    // The file cannot name what it does not contain, so the reference is null —
    // which loads, rather than refusing on an unknown name.
    const Runtime::BlueprintDefinition *definition = Runtime::GetBlueprintDefinition("crate.abp");
    REQUIRE(definition != nullptr);
    CHECK(definition->members[1].components.at("Parent").at("parent").is_null());
}
