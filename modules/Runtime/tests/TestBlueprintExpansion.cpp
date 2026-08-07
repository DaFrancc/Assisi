/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBlueprintExpansion.cpp
/// @brief Flattening a blueprint, placing it, and refusing the files that would
/// produce a world other than the one they describe.
///
/// The load-bearing property is that expansion is a pure function of the file's
/// bytes plus a placement: the member list's *order* becomes NetId assignment and
/// the composed transforms become a delta baseline, so a host and a client that
/// expand the same file must agree exactly
/// (docs/blueprint-system-concept.md §9). These cases pin the order, the
/// composition, the naming, and every refusal that keeps a file from meaning two
/// things at once.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/Naming.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

using namespace Assisi;
using Assisi::Runtime::BlueprintDefinition;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;

namespace
{

/// A fresh asset root per test case, so a cached definition from one case cannot
/// answer another's question.
std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_bp_" + name);
    std::error_code             ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

void Write(const std::filesystem::path &root, const std::string &name, const nlohmann::json &doc)
{
    std::ofstream out(root / name, std::ios::binary);
    out << doc.dump(2);
    REQUIRE(out.good());
}

nlohmann::json Entity(const std::string &name, nlohmann::json components = nlohmann::json::object())
{
    return {{"name", name}, {"components", std::move(components)}};
}

nlohmann::json At(float x, float y, float z)
{
    return {{"Transform",
             {{"position", {x, y, z}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}}}};
}

nlohmann::json Placement(float x, float y, float z, float scale = 1.f)
{
    return {{"position", {x, y, z}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {scale, scale, scale}}};
}

/// A car: a body, and a wheel parented to it.
nlohmann::json CarFile()
{
    return {{"version", 2},
            {"entities", nlohmann::json::array({Entity("body", At(0.f, 1.f, 0.f)),
                                                Entity("wheel_fl", {{"Transform",
                                                                     {{"position", {1.f, 0.f, 2.f}},
                                                                      {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                                      {"scale", {1.f, 1.f, 1.f}}}},
                                                                    {"Parent", {{"parent", "body"}}}})})}};
}

const ECS::Transform *TransformOf(ECS::Scene &scene, const InstanceTable &table, ECS::InstanceId instanceId,
                                  std::string_view memberName)
{
    const Runtime::BlueprintInstance *row = table.Find(instanceId);
    REQUIRE(row != nullptr);
    const BlueprintDefinition *definition = Runtime::GetBlueprintDefinition(row->source);
    REQUIRE(definition != nullptr);
    const std::optional<uint32_t> index = definition->IndexOf(memberName);
    REQUIRE(index.has_value());

    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        if (tag.instanceId == instanceId && tag.memberIndex == *index)
            return scene.Get<ECS::Transform>(entity);
    }
    return nullptr;
}

} // namespace

TEST_CASE("Blueprint: a file flattens to a member list in file order")
{
    const std::filesystem::path root = FreshRoot("flatten");
    Write(root, "car.abp", CarFile());

    const BlueprintDefinition *definition = Runtime::GetBlueprintDefinition("car.abp");
    REQUIRE(definition != nullptr);
    REQUIRE(definition->members.size() == 2);

    // File order, because it is NetId order on both machines.
    CHECK(definition->members[0].name == "body");
    CHECK(definition->members[1].name == "wheel_fl");
    CHECK_FALSE(definition->members[0].parented);
    CHECK(definition->members[1].parented);

    CHECK(definition->IndexOf("wheel_fl") == std::optional<uint32_t>{1});
    CHECK_FALSE(definition->IndexOf("nothing").has_value());

    // The closure warms what a spawn will need; for a leaf file it is itself.
    REQUIRE(definition->closure.size() == 1);
    CHECK(definition->closure[0] == "car.abp");
}

TEST_CASE("Blueprint: nesting flattens to one member list with path names")
{
    const std::filesystem::path root = FreshRoot("nesting");
    Write(root, "car.abp", CarFile());
    Write(root, "lot.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("sign", At(0.f, 0.f, 0.f))})},
           {"instances", nlohmann::json::array({{{"name", "car_1"},
                                                 {"source", "car.abp"},
                                                 {"transform", Placement(10.f, 0.f, 0.f)}},
                                                {{"name", "car_2"},
                                                 {"source", "car.abp"},
                                                 {"transform", Placement(20.f, 0.f, 0.f)}}})}});

    const BlueprintDefinition *definition = Runtime::GetBlueprintDefinition("lot.abp");
    REQUIRE(definition != nullptr);

    // One list, entities first then each instance's members — no tree, no inner
    // ids. The nested root evaporates exactly as the outer one does.
    REQUIRE(definition->members.size() == 5);
    CHECK(definition->members[0].name == "sign");
    CHECK(definition->members[1].name == "car_1/body");
    CHECK(definition->members[2].name == "car_1/wheel_fl");
    CHECK(definition->members[3].name == "car_2/body");
    CHECK(definition->members[4].name == "car_2/wheel_fl");

    // The nested placement is already composed into the member's own transform.
    CHECK(Runtime::TransformFromJson(definition->members[1].components.at("Transform")).position.x ==
          doctest::Approx(10.f));
    CHECK(Runtime::TransformFromJson(definition->members[3].components.at("Transform")).position.x ==
          doctest::Approx(20.f));

    // References follow the members they name: `body` inside car.abp is
    // `car_2/body` once the lot has flattened it, or the wheel would parent to
    // whichever "body" happened to answer first.
    CHECK(definition->members[4].components.at("Parent").at("parent").get<std::string>() == "car_2/body");

    CHECK(definition->closure == std::vector<std::string>{"car.abp", "lot.abp"});
}

TEST_CASE("Blueprint: a cycle is refused rather than expanded")
{
    const std::filesystem::path root = FreshRoot("cycle");
    Write(root, "a.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "b"}, {"source", "b.abp"}}})}});
    Write(root, "b.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array()},
           {"instances", nlohmann::json::array({{{"name", "a"}, {"source", "a.abp"}}})}});

    // Unrecoverable if missed: a containing b containing a expands forever.
    CHECK(Runtime::GetBlueprintDefinition("a.abp") == nullptr);
}

TEST_CASE("Blueprint: a non-uniform instance scale is refused, not clamped")
{
    const std::filesystem::path root = FreshRoot("scale");
    Write(root, "car.abp", CarFile());
    Write(root, "lot.abp", {{"version", 2},
                            {"entities", nlohmann::json::array()},
                            {"instances", nlohmann::json::array({{{"name", "squashed"},
                                                                  {"source", "car.abp"},
                                                                  {"transform",
                                                                   {{"position", {0.f, 0.f, 0.f}},
                                                                    {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                                    {"scale", {2.f, 1.f, 1.f}}}}}})}});

    // Clamping to an axis was rejected: it lets the file say one thing while the
    // game does another. Composing it is not an option either — the product is a
    // shear, which no Transform can represent.
    CHECK(Runtime::GetBlueprintDefinition("lot.abp") == nullptr);
}

TEST_CASE("Blueprint: a missing nested file leaves nothing behind")
{
    const std::filesystem::path root = FreshRoot("missing");
    Write(root, "lot.abp",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("sign", At(0.f, 0.f, 0.f))})},
           {"instances", nlohmann::json::array({{{"name", "car_1"}, {"source", "gone.abp"}}})}});

    CHECK(Runtime::GetBlueprintDefinition("lot.abp") == nullptr);

    ECS::Scene    scene;
    InstanceTable table;
    // All or nothing (§7): the sign is *before* the broken instance in the file,
    // and it must not survive the failure.
    CHECK_FALSE(SceneSerializer::ExpandInstance(scene, table, "lot.abp", ECS::Transform{}).has_value());
    CHECK(scene.AliveCount() == 0);
    CHECK(table.Size() == 0);
}

TEST_CASE("Blueprint: expanding places members, tags them, and records one table row")
{
    const std::filesystem::path root = FreshRoot("expand");
    Write(root, "car.abp", CarFile());

    ECS::Scene    scene;
    InstanceTable table;

    ECS::Transform placement;
    placement.position = {5.f, 0.f, 0.f};

    const std::optional<ECS::InstanceId> id =
        SceneSerializer::ExpandInstance(scene, table, "car.abp", placement);
    REQUIRE(id.has_value());
    CHECK(*id == ECS::InstanceId{1}); // ids start at 1; 0 is never a live instance

    const Runtime::BlueprintInstance *row = table.Find(*id);
    REQUIRE(row != nullptr);
    CHECK(row->source == "car.abp");
    CHECK(row->levelInstanceIndex == -1); // nothing placed it: a runtime spawn
    CHECK(row->name.empty());

    // Two members, both tagged with this instance and their own index.
    CHECK(scene.AliveCount() == 2);
    int32_t tagged = 0;
    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        CHECK(tag.instanceId == *id);
        CHECK(tag.memberIndex < 2);
        ++tagged;
    }
    CHECK(tagged == 2);

    // The placement reaches the parentless member directly...
    const ECS::Transform *body = TransformOf(scene, table, *id, "body");
    REQUIRE(body != nullptr);
    CHECK(body->position.x == doctest::Approx(5.f));
    CHECK(body->position.y == doctest::Approx(1.f));

    // ...and the parented one keeps its own offset, because the placement reaches
    // it through the parent. Composing again would apply it twice.
    const ECS::Transform *wheel = TransformOf(scene, table, *id, "wheel_fl");
    REQUIRE(wheel != nullptr);
    CHECK(wheel->position.x == doctest::Approx(1.f));
    CHECK(wheel->position.z == doctest::Approx(2.f));
}

TEST_CASE("Blueprint: a member's reference resolves to its own instance, not another's")
{
    const std::filesystem::path root = FreshRoot("refs");
    Write(root, "car.abp", CarFile());

    ECS::Scene    scene;
    InstanceTable table;

    const std::optional<ECS::InstanceId> first  = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    const std::optional<ECS::InstanceId> second = SceneSerializer::ExpandInstance(scene, table, "car.abp", {});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);

    // The whole reason references are qualified: two instances of one file, and
    // each wheel must find *its* body.
    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        if (tag.memberIndex != 1)
            continue; // only the wheel has a Parent

        const Runtime::Parent *parent = scene.Get<Runtime::Parent>(entity);
        REQUIRE(parent != nullptr);
        REQUIRE(parent->parent != ECS::NullEntity);

        const ECS::BlueprintMember *parentTag = scene.Get<ECS::BlueprintMember>(parent->parent);
        REQUIRE(parentTag != nullptr);
        CHECK(parentTag->instanceId == tag.instanceId);
        CHECK(parentTag->memberIndex == 0);
    }
}

TEST_CASE("Blueprint: a level's instances expand on load and are written back on save")
{
    const std::filesystem::path root = FreshRoot("level");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("spawn_marker", At(0.f, 0.f, 0.f))})},
           {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                 {"source", "car.abp"},
                                                 {"transform", Placement(22.f, 0.f, 4.f)}}})}});

    ECS::Scene           scene;
    InstanceTable        table;
    Runtime::LevelHeader header;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, &header, &table));

    // One authored entity plus the car's two members.
    CHECK(scene.AliveCount() == 3);
    REQUIRE(table.Size() == 1);
    REQUIRE(header.instances.size() == 1);
    CHECK(header.instances[0].name == "car_3");

    const Runtime::BlueprintInstance *row = table.Find(ECS::InstanceId{1});
    REQUIRE(row != nullptr);
    CHECK(row->name == "car_3");
    CHECK(row->levelInstanceIndex == 0);

    const ECS::Transform *body = TransformOf(scene, table, ECS::InstanceId{1}, "body");
    REQUIRE(body != nullptr);
    CHECK(body->position.x == doctest::Approx(22.f));
    CHECK(body->position.z == doctest::Approx(4.f));

    // Members carry their own file's name, not the path: the path is derived from
    // the tag, and would not fit in a Name anyway.
    bool sawWheelName = false;
    for (auto [entity, tag] : scene.Query<ECS::BlueprintMember>())
    {
        const Runtime::Name *name = scene.Get<Runtime::Name>(entity);
        REQUIRE(name != nullptr);
        sawWheelName = sawWheelName || name->value.View() == "wheel_fl";
    }
    CHECK(sawWheelName);

    // Saving writes the instance back and writes *no* member entities: baking the
    // car into the level is what would stop a fix to car.abp from propagating.
    const nlohmann::json saved = SceneSerializer::Save(scene, header, &table);
    REQUIRE(saved.at("entities").size() == 1);
    CHECK(saved.at("entities")[0].at("name").get<std::string>() == "spawn_marker");
    REQUIRE(saved.contains("instances"));
    REQUIRE(saved.at("instances").size() == 1);
    CHECK(saved.at("instances")[0].at("name").get<std::string>() == "car_3");
    CHECK(saved.at("instances")[0].at("source").get<std::string>() == "car.abp");
    CHECK(Runtime::TransformFromJson(saved.at("instances")[0].at("transform")).position.x ==
          doctest::Approx(22.f));
}

TEST_CASE("Blueprint: a level entity may point into an instance by path")
{
    const std::filesystem::path root = FreshRoot("crossref");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("trailer", {{"Parent", {{"parent", "car_3/body"}}}})})},
           {"instances", nlohmann::json::array({{{"name", "car_3"}, {"source", "car.abp"}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));

    const ECS::Entity trailer{.index = 0, .generation = 0};
    REQUIRE(scene.Get<Runtime::Name>(trailer) != nullptr);
    REQUIRE(scene.Get<Runtime::Name>(trailer)->value.View() == "trailer");

    const Runtime::Parent *parent = scene.Get<Runtime::Parent>(trailer);
    REQUIRE(parent != nullptr);
    REQUIRE(parent->parent != ECS::NullEntity);

    const ECS::BlueprintMember *tag = scene.Get<ECS::BlueprintMember>(parent->parent);
    REQUIRE(tag != nullptr);
    CHECK(tag->memberIndex == 0); // car_3/body
}

// --- One namespace, and what actually collides in it ------------------------
//
// Entity names and instance member paths are keys in one map (`nameToEntity`),
// which is what lets a level entity point at `car_3/body` at all. These four
// cases pin exactly where that shared namespace bites and where it does not —
// the editor's uniqueness guards are built to the answer, so if one of these
// ever flips, a guard is now either too weak or theatre.

TEST_CASE("Blueprint: an instance and an entity may share a name")
{
    // They read as a collision and are not one: the entity claims `car`, the
    // instance claims `car/body` and `car/wheel_fl`, and no reference can mean
    // both. Pinned because the obvious 'fix' — forbidding it — would reject
    // levels that are fine, and a guard nobody can justify is a guard that gets
    // widened until it hurts.
    const std::filesystem::path root = FreshRoot("nameshare");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("car", At(5.f, 0.f, 0.f))})},
           {"instances", nlohmann::json::array({{{"name", "car"}, {"source", "car.abp"}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));
    CHECK(table.Size() == 1);
}

TEST_CASE("Blueprint: two instances of one name are refused, not silently merged")
{
    // Both claim `car/body`. Reachable from the editor: CreateBlueprintFromSelection
    // names the instance after the file and checks nothing, so this is a level the
    // author can save and then never open again.
    const std::filesystem::path root = FreshRoot("dupinstance");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl", {{"version", 2},
                              {"entities", nlohmann::json::array()},
                              {"instances", nlohmann::json::array({{{"name", "car"}, {"source", "car.abp"}},
                                                                   {{"name", "car"}, {"source", "car.abp"}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));
}

TEST_CASE("Blueprint: an entity named like a member path is refused")
{
    // The one way an entity name and an instance name genuinely collide: the
    // entity spells the separator itself. Reachable from the Inspector's rename
    // box, which accepts any string.
    const std::filesystem::path root = FreshRoot("slashname");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl",
          {{"version", 2},
           {"entities", nlohmann::json::array({Entity("car/body", At(5.f, 0.f, 0.f))})},
           {"instances", nlohmann::json::array({{{"name", "car"}, {"source", "car.abp"}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));
}

TEST_CASE("Blueprint: two entities of one name are refused")
{
    // Reachable the same way, and the reason the rename box needs a guard at all.
    const std::filesystem::path root = FreshRoot("dupentity");
    Write(root, "main.alvl", {{"version", 2},
                              {"entities", nlohmann::json::array({Entity("crate", At(0.f, 0.f, 0.f)),
                                                                  Entity("crate", At(1.f, 0.f, 0.f))})}});

    ECS::Scene scene;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "main.alvl"));
}

TEST_CASE("Blueprint: placing a second instance under a live name is refused")
{
    // The guard sits here, at the one door both editor gestures come through,
    // rather than at each gesture — "Place instance" already uniquified and
    // "Create blueprint from selection" did not, which is exactly the shape of
    // bug a per-caller rule produces (round-7 S17). A refusal at the choke point
    // cannot be forgotten by the next caller.
    const std::filesystem::path root = FreshRoot("dupplace");
    Write(root, "car.abp", CarFile());

    ECS::Scene    scene;
    InstanceTable table;

    const Runtime::LevelInstance entry{
        .name = "car", .source = "car.abp", .transform = {}, .overrides = nlohmann::json::object(), .removed = {}};
    REQUIRE(SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/true).has_value());
    const std::size_t after_first = scene.AliveCount();

    CHECK_FALSE(SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/true).has_value());

    // Refused before anything was built, not unwound afterwards: a half-placed
    // instance that got cleaned up still burns entity slots the undo history has
    // handles into.
    CHECK(table.Size() == 1);
    CHECK(scene.AliveCount() == after_first);
}

TEST_CASE("Blueprint: unnamed instances may repeat — a spawn is not a name")
{
    // Runtime spawns and replicated mirrors pass no name (ExpandInstance and
    // BlueprintReplication both leave it empty), and nothing addresses their
    // members by path. A hundred bullets are not a hundred collisions, and a
    // guard that did not say so would break replication outright.
    const std::filesystem::path root = FreshRoot("unnamed");
    Write(root, "car.abp", CarFile());

    ECS::Scene    scene;
    InstanceTable table;

    const Runtime::LevelInstance entry{
        .name = {}, .source = "car.abp", .transform = {}, .overrides = nlohmann::json::object(), .removed = {}};
    CHECK(SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/false).has_value());
    CHECK(SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/false).has_value());
    CHECK(table.Size() == 2);
}

TEST_CASE("Blueprint: UniqueInstanceName steps past the names already taken")
{
    // What the editor calls before placing, so the author gets `car_1` rather
    // than a refusal. The refusal above is the backstop; this is the manners.
    const std::filesystem::path root = FreshRoot("uniquename");
    Write(root, "car.abp", CarFile());

    ECS::Scene    scene;
    InstanceTable table;

    CHECK(Runtime::UniqueInstanceName(table, "car") == "car");

    const auto place = [&](const std::string &name)
    {
        const Runtime::LevelInstance entry{.name      = name,
                                           .source    = "car.abp",
                                           .transform = {},
                                           .overrides = nlohmann::json::object(),
                                           .removed   = {}};
        REQUIRE(SceneSerializer::PlaceInstance(scene, table, entry, /*authored=*/true).has_value());
    };

    place("car");
    CHECK(Runtime::UniqueInstanceName(table, "car") == "car_1");

    place("car_1");
    CHECK(Runtime::UniqueInstanceName(table, "car") == "car_2");

    // The suffix walk skips what is taken rather than counting rows: `car_2` is
    // free here even though three instances are live.
    place("car_3");
    CHECK(Runtime::UniqueInstanceName(table, "car") == "car_2");
}

TEST_CASE("Naming: the separator is the one character a name may not hold")
{
    CHECK(Runtime::ValidateName("car").has_value());
    CHECK(Runtime::ValidateName("car_3").has_value());
    CHECK(Runtime::ValidateName("Entity 12").has_value()); // spaces are fine; only '/' addresses

    REQUIRE_FALSE(Runtime::ValidateName("").has_value());
    CHECK(Runtime::ValidateName("").error() == Runtime::NameError::Empty);

    REQUIRE_FALSE(Runtime::ValidateName("car/body").has_value());
    CHECK(Runtime::ValidateName("car/body").error() == Runtime::NameError::ContainsSeparator);
    // Leading and trailing count too — `/car` is how an override addresses the
    // writing file, so a name spelling one would be read as an address.
    CHECK(Runtime::ValidateName("/car").error() == Runtime::NameError::ContainsSeparator);

    REQUIRE_FALSE(Runtime::ValidateName(std::string(Core::kShortStringMax + 1, 'a')).has_value());
    CHECK(Runtime::ValidateName(std::string(Core::kShortStringMax + 1, 'a')).error() ==
          Runtime::NameError::TooLong);
    CHECK(Runtime::ValidateName(std::string(Core::kShortStringMax, 'a')).has_value()); // the limit itself fits
}

TEST_CASE("Naming: an entity name walks past what is taken, and knows itself")
{
    ECS::Scene scene;

    const auto name = [&scene](std::string_view value)
    {
        const ECS::Entity e = scene.Create();
        (void)scene.Add(e, Runtime::Name{Core::ShortString{value}});
        return e;
    };

    CHECK(Runtime::UniqueEntityName(scene, "Entity") == "Entity");

    const ECS::Entity first = name("Entity");
    CHECK(Runtime::UniqueEntityName(scene, "Entity") == "Entity_1");

    name("Entity_1");
    CHECK(Runtime::UniqueEntityName(scene, "Entity") == "Entity_2");

    // An entity does not conflict with itself: renaming `Entity` to `Entity` is a
    // no-op, not a refusal, which is what keeps the rename box from rejecting the
    // name already in its own field.
    CHECK(Runtime::EntityNameTaken(scene, "Entity"));
    CHECK_FALSE(Runtime::EntityNameTaken(scene, "Entity", first));

    // "No name" is not a name — any number of entities may have none.
    CHECK_FALSE(Runtime::EntityNameTaken(scene, ""));
}

TEST_CASE("Blueprint: an entity named with the separator is refused on its own")
{
    // Not only when an instance happens to collide with it: the name is malformed
    // whatever else the level holds, and refusing it here is what lets everything
    // downstream assume entity names and member paths cannot overlap.
    const std::filesystem::path root = FreshRoot("slashonly");
    Write(root, "main.alvl",
          {{"version", 2}, {"entities", nlohmann::json::array({Entity("car/body", At(0.f, 0.f, 0.f))})}});

    ECS::Scene scene;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "main.alvl"));
}

TEST_CASE("Blueprint: a level with instances refuses to load with nowhere to record them")
{
    const std::filesystem::path root = FreshRoot("notable");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl", {{"version", 2},
                              {"entities", nlohmann::json::array()},
                              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                                    {"source", "car.abp"}}})}});

    // Dropping them instead would produce a level missing most of its content,
    // reported as a successful load.
    ECS::Scene scene;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "main.alvl"));
}

TEST_CASE("Blueprint: instance ids restart with the world")
{
    const std::filesystem::path root = FreshRoot("ids");
    Write(root, "car.abp", CarFile());
    Write(root, "main.alvl", {{"version", 2},
                              {"entities", nlohmann::json::array()},
                              {"instances", nlohmann::json::array({{{"name", "car_3"},
                                                                    {"source", "car.abp"}}})}});

    ECS::Scene    scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));
    CHECK(table.Find(ECS::InstanceId{1}) != nullptr);

    // Not stable across a load, and nothing may assume they are — so the second
    // load hands out 1 again rather than 2.
    REQUIRE(SceneSerializer::LoadFromFile(scene, "main.alvl", {}, nullptr, &table));
    CHECK(table.Size() == 1);
    CHECK(table.Find(ECS::InstanceId{1}) != nullptr);
    CHECK(table.Find(ECS::InstanceId{2}) == nullptr);
}

TEST_CASE("Blueprint: composition is a placement applied to a local pose")
{
    // Named separately because both sides of the wire call this one function, and
    // two spellings that differ in the low bits are a silent cross-build desync
    // rather than a visible bug.
    ECS::Transform placement;
    placement.position = {10.f, 0.f, 0.f};
    placement.rotation = glm::angleAxis(glm::radians(90.f), glm::vec3(0.f, 1.f, 0.f));
    placement.scale    = {2.f, 2.f, 2.f};

    ECS::Transform local;
    local.position = {1.f, 0.f, 0.f};

    const ECS::Transform composed = Runtime::ComposeTransform(placement, local);
    // Scaled, then rotated, then translated: (1,0,0) * 2 turned 90° about Y is
    // (0,0,-2), landing at (10,0,-2).
    CHECK(composed.position.x == doctest::Approx(10.f));
    CHECK(composed.position.z == doctest::Approx(-2.f));
    CHECK(composed.scale.x == doctest::Approx(2.f));

    CHECK(Runtime::HasUniformScale(placement));
    CHECK_FALSE(Runtime::HasUniformScale(ECS::Transform{.scale = {1.f, 2.f, 1.f}}));
}
