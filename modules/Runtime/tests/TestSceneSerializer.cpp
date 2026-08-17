/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

// doctest forward-declares std::ostream; stringifying a std::string_view in a
// CHECK (a Name's View() comparison) needs the complete type.
#include <ostream>

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include <set>
#include <string>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>

// Private to the serializer's own translation units: the nesting tests at the
// bottom engage a context directly, which is the one thing the public API cannot
// do. No include path covers it, so it is reached by relative path.
#include "../src/SceneSerializerContext.hpp"

using namespace Assisi;
using Assisi::Runtime::Camera;
using Assisi::Runtime::DirectionalLight;
using Assisi::Runtime::LevelError;
using Assisi::Runtime::LevelResult;
using Assisi::Runtime::MeshRenderer;
using Assisi::Runtime::Parent;
using Assisi::Runtime::PointLight;
using Assisi::Runtime::SceneSerializer;
using Assisi::Runtime::SpotLight;
using Assisi::Runtime::Transform;

TEST_CASE("SceneSerializer: transform values survive a round-trip")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 2.f, 3.f},
                                   .rotation = glm::quat{1.f, 0.f, 0.f, 0.f},
                                   .scale    = {4.f, 5.f, 6.f}}) != nullptr);

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, SceneSerializer::Save(scene)).has_value());

    const auto *t = loaded.Get<Transform>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(1.f));
    CHECK(t->position.y == doctest::Approx(2.f));
    CHECK(t->position.z == doctest::Approx(3.f));
    CHECK(t->scale.x == doctest::Approx(4.f));
    CHECK(t->scale.z == doctest::Approx(6.f));
}

// Regression for the forward-reference bug: a child whose serial index precedes
// its parent's must still resolve the parent on load. Creating the child before
// the parent makes the child sort first (lower key), so its Parent
// references an entity that a single-pass loader has not created yet.
TEST_CASE("SceneSerializer: forward parent reference survives a round-trip")
{
    ECS::Scene scene;
    const ECS::Entity child  = scene.Create(); // {0,0} — sorts first
    const ECS::Entity parent = scene.Create(); // {1,0}

    REQUIRE(scene.Add(parent, Transform{}) != nullptr);
    REQUIRE(scene.Add(child, Transform{}) != nullptr);
    REQUIRE(scene.Add(child, Parent{.parent = parent}) != nullptr);

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, SceneSerializer::Save(scene)).has_value());

    // Load allocates fresh sequential entities: serial 0 -> {0,0} (the child),
    // serial 1 -> {1,0} (the parent).
    const ECS::Entity loadedChild{.index = 0, .generation = 0};
    const ECS::Entity loadedParent{.index = 1, .generation = 0};

    const auto *pc = loaded.Get<Parent>(loadedChild);
    REQUIRE(pc != nullptr);
    CHECK(pc->parent != ECS::NullEntity); // the bug flattened this to null
    CHECK(pc->parent == loadedParent);
}

// Adversarial hand-authored fixture: entity[0] (the child) references "body",
// which appears *after* it in the array. Exercises the loader directly,
// independent of save ordering.
TEST_CASE("SceneSerializer: child-before-parent fixture loads the hierarchy")
{
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities",
         nlohmann::json::array(
             {{{"name", "wheel"}, {"components", {{"Parent", {{"parent", "body"}}}}}}, // [0] -> body
                 {{"name", "body"},
                     {"components", {{"Transform",
                          {{"position", {0.f, 0.f, 0.f}},
                              {"rotation", {1.f, 0.f, 0.f, 0.f}},
                              {"scale", {1.f, 1.f, 1.f}}}}}}}})}};    // [1] body

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, fixture).has_value());

    const auto *pc = loaded.Get<Parent>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(pc != nullptr);
    CHECK(pc->parent == ECS::Entity{.index = 1, .generation = 0});
}

// ---------------------------------------------------------------------------
// Names (format v2). A name is what an override and every EntityRef address an
// entity by, so a file whose names are ambiguous or dangling means something
// other than what it says — and every one of these refuses rather than guesses.
// ---------------------------------------------------------------------------

TEST_CASE("SceneSerializer: a name survives the round trip and is the entity's Name")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);
    REQUIRE(scene.Add(e, Runtime::Name{Core::EntityName{"wheel_fl"}}) != nullptr);

    const nlohmann::json saved = SceneSerializer::Save(scene);
    REQUIRE(saved.at("version").get<int32_t>() == 2);
    REQUIRE(saved.at("entities").size() == 1);
    CHECK(saved.at("entities")[0].at("name").get<std::string>() == "wheel_fl");
    // Written once, at the entity level — not a second time inside components.
    CHECK_FALSE(saved.at("entities")[0].at("components").contains("Name"));

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, saved).has_value());
    const auto *name = loaded.Get<Runtime::Name>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(name != nullptr);
    CHECK(name->value.View() == "wheel_fl");
}

TEST_CASE("SceneSerializer: an unnamed entity is given a name, and duplicates are disambiguated")
{
    ECS::Scene scene;
    const ECS::Entity a = scene.Create();
    const ECS::Entity b = scene.Create();
    const ECS::Entity c = scene.Create();
    REQUIRE(scene.Add(a, Transform{}) != nullptr);
    REQUIRE(scene.Add(b, Transform{}) != nullptr);
    REQUIRE(scene.Add(c, Transform{}) != nullptr);
    // Two entities really can share a Name — it has always been a free-form label.
    REQUIRE(scene.Add(a, Runtime::Name{Core::EntityName{"Cube"}}) != nullptr);
    REQUIRE(scene.Add(b, Runtime::Name{Core::EntityName{"Cube"}}) != nullptr);

    const nlohmann::json saved = SceneSerializer::Save(scene);
    const auto &list  = saved.at("entities");
    REQUIRE(list.size() == 3);

    std::set<std::string> names;
    for (const auto &entity : list)
        names.insert(entity.at("name").get<std::string>());
    CHECK(names.size() == 3); // unique, which is what makes the file loadable at all
    CHECK(names.contains("Cube"));
    CHECK(names.contains("Cube_1"));

    // And it reloads, which a file with two "Cube"s would not.
    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, saved).has_value());
    CHECK(loaded.AliveCount() == 3);

    // Deterministic: the same scene saves to the same names, so an override written
    // against one save still addresses the same entity after the next.
    CHECK(SceneSerializer::Save(scene) == saved);
}

TEST_CASE("SceneSerializer: two entities with the same name refuse the file")
{
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "body"}, {"components", nlohmann::json::object()}},
                                               {{"name", "body"}, {"components", nlohmann::json::object()}}})}};

    ECS::Scene loaded;
    const LevelResult result = SceneSerializer::Load(loaded, fixture);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::DuplicateName);
    CHECK(loaded.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: a missing or empty name refuses the file")
{
    ECS::Scene loaded;

    // Absent and present-but-unusable are different kinds, and the file is wrong
    // in a different way in each: MissingName means the key is not there at all,
    // InvalidName means ValidateName looked at it and said no.
    const nlohmann::json missing = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"components", nlohmann::json::object()}}})}};
    const LevelResult noName = SceneSerializer::Load(loaded, missing);
    REQUIRE_FALSE(noName.has_value());
    CHECK(noName.error() == LevelError::MissingName);

    const nlohmann::json empty = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", ""}, {"components", nlohmann::json::object()}}})}};
    const LevelResult emptyName = SceneSerializer::Load(loaded, empty);
    REQUIRE_FALSE(emptyName.has_value());
    CHECK(emptyName.error() == LevelError::InvalidName);

    // Truncating instead is how two members become indistinguishable.
    const nlohmann::json tooLong = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", std::string(Core::kEntityNameMax + 1, 'x')},
                                               {"components", nlohmann::json::object()}}})}};
    const LevelResult longName = SceneSerializer::Load(loaded, tooLong);
    REQUIRE_FALSE(longName.has_value());
    CHECK(longName.error() == LevelError::InvalidName);
}

TEST_CASE("SceneSerializer: a reference to an undeclared name refuses the file")
{
    // The failure names exist to prevent, in its most direct form: a Parent
    // pointing at something the file never declares. Nulling it would flatten the
    // hierarchy silently, which is what the positional format already did.
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "wheel"}, {"components", {{"Parent", {{"parent", "chassis"}}}}}},
                                               {{"name", "body"}, {"components", nlohmann::json::object()}}})}};

    ECS::Scene loaded;
    const LevelResult result = SceneSerializer::Load(loaded, fixture);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::UnresolvedReference);
    CHECK(loaded.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: a v1 file is refused rather than read positionally")
{
    // Its refs are numbers, which under v2 would each resolve to *some* entity if
    // the loader guessed. There is no v1 reader and this is what that means.
    const nlohmann::json v1 = {
        {"version", 1},
        {"entities", nlohmann::json::array({{{"components", {{"Parent", {{"parent", 1}}}}}},
                                               {{"components", nlohmann::json::object()}}})}};

    ECS::Scene loaded;
    const LevelResult result = SceneSerializer::Load(loaded, v1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::UnsupportedVersion); // the version check refuses it first
    CHECK(loaded.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: a null reference stays null")
{
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "loose"}, {"components", {{"Parent", {{"parent", nullptr}}}}}}})}};

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, fixture).has_value());
    const auto *pc = loaded.Get<Parent>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(pc != nullptr);
    CHECK(pc->parent == ECS::NullEntity);
}

TEST_CASE("SceneSerializer: an empty scene round-trips to an empty scene")
{
    ECS::Scene scene;
    ECS::Scene loaded;
    loaded.Create(); // pre-existing entity that Load must clear away

    REQUIRE(SceneSerializer::Load(loaded, SceneSerializer::Save(scene)).has_value());
    CHECK(loaded.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: MeshRenderer asset ids round-trip; GPU handles don't")
{
    // Reserved built-in id for prim://cube, plus an arbitrary material id. No hint
    // resolver is installed, so the GUIDs serialize without a path hint and the
    // ids round-trip on their own.
    const Core::AssetId cubeId     = Core::BuiltinAssetId::Cube;
    const Core::AssetId materialId = *Core::AssetId::Parse("8c08e9c0-e9fb-4f84-a9ba-7a90223526fd");

    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    // Built field-by-field rather than as an aggregate: MeshRenderer's two
    // transient members (meshBuffer, materials) are runtime caches this test has
    // no business naming, and omitting them from a braced initializer draws
    // -Wmissing-field-initializers.
    MeshRenderer renderer;
    renderer.mesh              = cubeId;
    renderer.materialOverrides = {materialId};
    REQUIRE(scene.Add(e, renderer) != nullptr);

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, SceneSerializer::Save(scene)).has_value());

    const MeshRenderer *mrc =
        loaded.Get<MeshRenderer>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(mrc != nullptr); // presence survives
    CHECK(mrc->mesh == cubeId);
    REQUIRE(mrc->materialOverrides.size() == 1);
    CHECK(mrc->materialOverrides[0] == materialId);
    // The mesh/material handles are transient: never serialized, so they come
    // back empty and get re-resolved from the ids at load time.
    CHECK(mrc->meshBuffer == nullptr);
    CHECK(mrc->materials.empty());
}

TEST_CASE("SceneSerializer: shadow-casting flags round-trip on every caster type")
{
    // Only the OFF value proves anything: ON is the default, so a flag that never
    // serialized at all would still read back ON and pass.
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, DirectionalLight{.castsShadows = false}) != nullptr);
    REQUIRE(scene.Add(e, PointLight{.castsShadows = false}) != nullptr);
    REQUIRE(scene.Add(e, SpotLight{.castsShadows = false}) != nullptr);
    MeshRenderer renderer;
    renderer.castsShadows = false;
    REQUIRE(scene.Add(e, renderer) != nullptr);

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, SceneSerializer::Save(scene)).has_value());

    const ECS::Entity le{.index = 0, .generation = 0};
    REQUIRE(loaded.Get<DirectionalLight>(le) != nullptr);
    REQUIRE(loaded.Get<PointLight>(le) != nullptr);
    REQUIRE(loaded.Get<SpotLight>(le) != nullptr);
    REQUIRE(loaded.Get<MeshRenderer>(le) != nullptr);
    CHECK(loaded.Get<DirectionalLight>(le)->castsShadows == false);
    CHECK(loaded.Get<PointLight>(le)->castsShadows == false);
    CHECK(loaded.Get<SpotLight>(le)->castsShadows == false);
    CHECK(loaded.Get<MeshRenderer>(le)->castsShadows == false);
}

TEST_CASE("SceneSerializer: a level saved before the shadow flags existed casts shadows")
{
    // The compatibility half: every level in the tree predates these fields, and
    // each one must load as a shadow caster rather than as silently unshadowed.
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "lamp"},
                                               {"components",
                                                {{"PointLight", {{"intensity", 3.f}}},
                                                    {"MeshRenderer", nlohmann::json::object()}}}}})}};

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, fixture).has_value());

    const ECS::Entity le{.index = 0, .generation = 0};
    const PointLight *light      = loaded.Get<PointLight>(le);
    const MeshRenderer *renderer = loaded.Get<MeshRenderer>(le);
    REQUIRE(light != nullptr);
    REQUIRE(renderer != nullptr);
    CHECK(light->intensity == doctest::Approx(3.f));
    CHECK(light->castsShadows == true);
    CHECK(renderer->castsShadows == true);
}

TEST_CASE("SceneSerializer: multiple components on one entity all round-trip")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 2.f, 3.f}}) != nullptr);
    REQUIRE(scene.Add(e, Camera{.fovDegrees = 42.f, .isActive = true}) != nullptr);

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, SceneSerializer::Save(scene)).has_value());

    const ECS::Entity le{.index = 0, .generation = 0};
    const auto *t = loaded.Get<Transform>(le);
    const auto *c = loaded.Get<Camera>(le);
    REQUIRE(t != nullptr);
    REQUIRE(c != nullptr);
    CHECK(t->position.x == doctest::Approx(1.f));
    CHECK(c->fovDegrees == doctest::Approx(42.f));
    CHECK(c->isActive == true);
}

TEST_CASE("SceneSerializer: an unsupported version leaves the scene untouched")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{}) != nullptr);

    const nlohmann::json future = {{"version", 999}, {"entities", nlohmann::json::array()}};
    const LevelResult result = SceneSerializer::Load(scene, future);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::UnsupportedVersion); // rejected before the scene is cleared

    CHECK(scene.AliveCount() == 1);
    CHECK(scene.Get<Transform>(e) != nullptr);
}

// ---------------------------------------------------------------------------
// Which side of the clear a refusal landed on (round-7 B20).
//
// "The load failed" is not enough for a caller that holds anything *about* the
// scene — an editor's undo stacks, its selection, its pick targets are all
// entity handles. A refusal before the clear leaves those valid; one after it
// leaves every handle aliasing a live but different entity in a scene rebuilt
// densely from {0,0}. The error kind cannot answer that question (MissingName
// is returned from both sides of the clear), so the load reports it directly.
// ---------------------------------------------------------------------------

TEST_CASE("SceneSerializer: a refusal before the clear says the scene was not replaced")
{
    ECS::Scene scene;
    const ECS::Entity kept = scene.Create();
    REQUIRE(scene.Add(kept, Transform{}) != nullptr);

    SUBCASE("an unreadable version")
    {
        const nlohmann::json future = {{"version", 999}, {"entities", nlohmann::json::array()}};
        const LevelResult result = SceneSerializer::Load(scene, future);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == LevelError::UnsupportedVersion);
        CHECK_FALSE(result.error().sceneReplaced);
    }

    SUBCASE("instances with nowhere to put them")
    {
        // Read before anything is destroyed, precisely so a caller with no table
        // does not pay for the file with the level it had.
        const nlohmann::json fixture = {
            {"version", 2},
            {"entities", nlohmann::json::array()},
            {"instances", nlohmann::json::array({{{"name", "car_0"}, {"source", "car.abp"}}})}};
        const LevelResult result = SceneSerializer::Load(scene, fixture);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == LevelError::NoInstanceTable);
        CHECK_FALSE(result.error().sceneReplaced);
    }

    // Either way the caller still has the level it walked in with.
    CHECK(scene.AliveCount() == 1);
    CHECK(scene.Get<Transform>(kept) != nullptr);
}

TEST_CASE("SceneSerializer: a refusal after the clear says the scene was replaced")
{
    ECS::Scene scene;
    const ECS::Entity doomed = scene.Create();
    REQUIRE(scene.Add(doomed, Transform{}) != nullptr);

    // Both of these are refused *past* the point where Load committed to building
    // a scene, and both leave an empty one behind. A caller told only "it failed"
    // is the B20 bug: it goes on believing its handles describe something.
    nlohmann::json fixture;
    SUBCASE("two entities under one name")
    {
        fixture = {{"version", 2},
            {"entities", nlohmann::json::array({{{"name", "body"}, {"components", nlohmann::json::object()}},
                                                   {{"name", "body"}, {"components", nlohmann::json::object()}}})}};
    }
    SUBCASE("a component field of the wrong type")
    {
        fixture = {{"version", 2},
            {"entities", nlohmann::json::array({{{"name", "eye"},
                                                   {"components", {{"Camera", {{"fovDegrees", "wide"}}}}}}})}};
    }

    const LevelResult result = SceneSerializer::Load(scene, fixture);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().sceneReplaced);

    // And it means it: the entity the caller held is gone, not merely suspect.
    CHECK(scene.AliveCount() == 0);
    CHECK_FALSE(scene.IsAlive(doomed));
}

// ---------------------------------------------------------------------------
// A malformed *shape* is refused by value, not thrown.
//
// Load's contract is std::expected-shaped: every way a file can be wrong comes
// back as a LevelError. Left unguarded, a `version` that is a string or a missing
// `entities` array reaches nlohmann and throws from *past* the clear, leaving the
// caller an emptied scene and no error return. The disk paths would hide that
// behind their own try/catch; a caller handing Load an already-parsed json (as
// these tests do) wears it.
//
// A throw here fails the test outright, which is the behaviour under test.
// ---------------------------------------------------------------------------

TEST_CASE("SceneSerializer: a malformed top level is refused, and the scene survives")
{
    ECS::Scene scene;
    const ECS::Entity kept = scene.Create();
    REQUIRE(scene.Add(kept, Transform{}) != nullptr);

    nlohmann::json fixture;
    SUBCASE("a version that is not a number")
    {
        // Reaches j.value("version", 0), which converts rather than checking.
        fixture = {{"version", "2"}, {"entities", nlohmann::json::array()}};
    }
    SUBCASE("no entities key at all")
    {
        // The at() that gave this issue its name.
        fixture = {{"version", 2}};
    }
    SUBCASE("an entities key that is not an array")
    {
        // size() answers on an object too, so this got as far as entities[0].
        fixture = {{"version", 2}, {"entities", {{"body", 1}}}};
    }
    SUBCASE("a document that is not an object")
    {
        fixture = nlohmann::json::array({1, 2});
    }

    const LevelResult result = SceneSerializer::Load(scene, fixture);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::MalformedJson);

    // All four are read before the clear, so the caller keeps the level it had —
    // the same bargain the version-mismatch refusal already made.
    CHECK_FALSE(result.error().sceneReplaced);
    CHECK(scene.AliveCount() == 1);
    CHECK(scene.Get<Transform>(kept) != nullptr);
}

TEST_CASE("SceneSerializer: a components key of the wrong shape is refused, not thrown")
{
    ECS::Scene scene;
    const ECS::Entity doomed = scene.Create();
    REQUIRE(scene.Add(doomed, Transform{}) != nullptr);

    // Presence is not shape, and this one never throws: items() over a primitive
    // yields a single empty key, so unguarded, every component the entity declares
    // is dropped as "unknown" and the load reports success. Silent, which is worse
    // than the throws above. Found in pass 3, past the clear by construction, so it
    // reports the replacement rather than pretending the scene survived.
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "body"}, {"components", "Transform"}}})}};

    const LevelResult result = SceneSerializer::Load(scene, fixture);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::MalformedJson);
    CHECK(result.error().sceneReplaced);
    CHECK(scene.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: loading through a file reports the replacement too")
{
    namespace fs        = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_replaced_test";
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    {
        std::ofstream bad(root / "unparseable.alvl");
        bad << "{ this is not valid json";
    }
    {
        // Valid JSON, right version, no `entities`. Load guards its own top-level
        // reads and refuses before the clear, which is why this file is
        // grouped with the parse failure rather than with the replacement below.
        // Unguarded it throws instead, and LoadFromFile's catch clears the scene on
        // the way out — a shape Load could see coming costing the caller its level.
        std::ofstream headless(root / "no_entities.alvl");
        headless << R"({"version": 2})";
    }
    {
        // A refusal that genuinely lands past the clear, so the replacement half
        // of this test still has a subject: the duplicate is only discoverable
        // once Load is building the scene.
        std::ofstream twins(root / "twins.alvl");
        twins << R"({"version": 2, "entities": [{"name": "body"}, {"name": "body"}]})";
    }

    ECS::Scene scene;
    const ECS::Entity kept = scene.Create();
    REQUIRE(scene.Add(kept, Transform{}) != nullptr);

    const LevelResult unparseable = SceneSerializer::LoadFromFile(scene, "unparseable.alvl");
    REQUIRE_FALSE(unparseable.has_value());
    CHECK_FALSE(unparseable.error().sceneReplaced);
    CHECK(scene.AliveCount() == 1); // never reached the clear

    const LevelResult headless = SceneSerializer::LoadFromFile(scene, "no_entities.alvl");
    REQUIRE_FALSE(headless.has_value());
    CHECK(headless.error() == LevelError::MalformedJson);
    CHECK_FALSE(headless.error().sceneReplaced);
    CHECK(scene.AliveCount() == 1); // the guard is what keeps this 1

    // The error kind cannot answer which side of the clear a failure landed on —
    // both refusals above report MalformedJson and neither touches the scene — so
    // the load reports the replacement separately.
    const LevelResult replaced = SceneSerializer::LoadFromFile(scene, "twins.alvl");
    REQUIRE_FALSE(replaced.has_value());
    CHECK(replaced.error() == LevelError::DuplicateName);
    CHECK(replaced.error().sceneReplaced);
    CHECK(scene.AliveCount() == 0);
}

// ---------------------------------------------------------------------------
// Component field values. A `contains()` guard proves the key is there and says
// nothing about its type, so the generated deserializers check the type as well
// and refuse the file by name — otherwise a mistyped field throws out of nlohmann.
// ---------------------------------------------------------------------------

TEST_CASE("SceneSerializer: a field of the wrong type refuses the file rather than throwing")
{
    // The key is present, so a contains()-only guard lets it through.
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "eye"},
                                               {"components", {{"Camera", {{"fovDegrees", "wide"}}}}}}})}};

    ECS::Scene loaded;
    LevelResult result;
    CHECK_NOTHROW(result = SceneSerializer::Load(loaded, fixture));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::MalformedComponent);

    // Refused, not partly applied: a level with one bad float must not come back
    // as a level missing one component and holding every other.
    CHECK(loaded.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: an array field of the wrong length refuses the file")
{
    // Two elements where three go. Without the size check the codegen walks off
    // _v[2] and throws from inside glm's initializer.
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities",
         nlohmann::json::array({{{"name", "body"},
                                   {"components", {{"Transform", {{"position", {0.f, 0.f, 0.f}},
                                            {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                            {"scale", {1.f, 1.f}}}}}}}})}};

    ECS::Scene loaded;
    LevelResult result;
    CHECK_NOTHROW(result = SceneSerializer::Load(loaded, fixture));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::MalformedComponent);
    CHECK(loaded.AliveCount() == 0);
}

TEST_CASE("SceneSerializer: an absent field is not a failure and keeps its default")
{
    // The other half of the rule, and the one that has to stay silent: this is
    // how a component gains a field without refusing every level saved before it.
    // A file that simply does not mention `isActive` is an ordinary old file.
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "eye"},
                                               {"components", {{"Camera", {{"fovDegrees", 42.f}}}}}}})}};

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, fixture).has_value());

    const Camera *camera = loaded.Get<Camera>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(camera != nullptr);
    CHECK(camera->fovDegrees == doctest::Approx(42.f));
    CHECK(camera->isActive == Camera{}.isActive); // the C++ default, silently and correctly
}

TEST_CASE("SceneSerializer: a version mismatch through a file fails the load, not just the log")
{
    // Logging alone is not enough: LoadFromFile would return true over an empty
    // scene, so a level from a newer build reads as a level with nothing in it —
    // all the way up to the server announcing it had loaded the world.
    namespace fs        = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_version_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    {
        std::ofstream out(root / "old.alvl", std::ios::binary);
        out << nlohmann::json{{"version", 1}, {"entities", nlohmann::json::array()}}.dump(2);
    }

    ECS::Scene scene;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "old.alvl"));
    fs::remove_all(root, ec);
}

TEST_CASE("SceneSerializer: save/load through a file round-trips")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_test";
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {8.f, 0.f, 0.f}}) != nullptr);

    REQUIRE(SceneSerializer::SaveToFile(scene, root / "rt.alvl"));

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::LoadFromFile(loaded, "rt.alvl"));
    const auto *t = loaded.Get<Transform>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(8.f));
}

TEST_CASE("SceneSerializer: a saved level is LF on every platform")
{
    // Levels are content-hashed to decide whether two machines are running the
    // same world, so the bytes have to be a function of the scene and nothing
    // else. A text-mode write expands '\n' to "\r\n" on Windows only, which made
    // a Windows and a Linux editor refuse to pair over a level neither had
    // touched. This fails on Windows the moment SaveToFile drops std::ios::binary
    // — the point of asserting on raw bytes rather than on a round-trip, which
    // would keep passing either way.
    namespace fs            = std::filesystem;
    const fs::path root     = fs::temp_directory_path() / "assisi_serializer_eol_test";
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(scene.Add(e, Transform{.position = {1.f, 2.f, 3.f}}) != nullptr);

    const fs::path file = root / "eol.alvl";
    REQUIRE(SceneSerializer::SaveToFile(scene, file));

    std::ifstream in(file, std::ios::binary);
    REQUIRE(in.is_open());
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Pretty-printed JSON, so there are newlines to get wrong in the first place.
    CHECK(bytes.find('\n') != std::string::npos);
    CHECK(bytes.find('\r') == std::string::npos);
}

TEST_CASE("SceneSerializer: malformed and missing files fail cleanly, not fatally")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_test";
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    {
        std::ofstream bad(root / "bad.alvl");
        bad << "{ this is not valid json";
    }

    ECS::Scene scene;
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "bad.alvl"));      // parse error caught
    CHECK_FALSE(SceneSerializer::LoadFromFile(scene, "nope.alvl"));     // missing file
}

TEST_CASE("SceneSerializer: unknown component names are skipped, not fatal")
{
    const nlohmann::json fixture = {
        {"version", 2},
        {"entities", nlohmann::json::array({{{"name", "thing"},
                                               {"components",
                                                {{"NopeComponent", {{"x", 1}}},
                                                    {"Transform",
                                                     {{"position", {7.f, 0.f, 0.f}},
                                                         {"rotation", {1.f, 0.f, 0.f, 0.f}},
                                                         {"scale", {1.f, 1.f, 1.f}}}}}}}})}};

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, fixture).has_value());

    const auto *t = loaded.Get<Transform>(ECS::Entity{.index = 0, .generation = 0});
    REQUIRE(t != nullptr);
    CHECK(t->position.x == doctest::Approx(7.f));
}

// ---------------------------------------------------------------------------
// ScopedRawEntityContext — the identity mapping the editor undo/redo system uses
// to serialize/restore a single component's EntityRef fields outside Save/Load.
// ---------------------------------------------------------------------------

TEST_CASE("SceneSerializer: raw-entity context round-trips a Parent handle outside Save/Load")
{
    ECS::Scene scene;
    const ECS::Entity a = scene.Create(); // the parent
    const ECS::Entity b = scene.Create(); // the child, references a
    REQUIRE(scene.Add(a, Transform{}) != nullptr);
    REQUIRE(scene.Add(b, Parent{.parent = a}) != nullptr);

    const auto *meta = Core::Reflect::ComponentRegistry::Instance().Find("Parent");
    REQUIRE(meta != nullptr);

    // Capture: serialize b's Parent to JSON under the raw context. The handle is
    // encoded as a's raw slot index — not collapsed, not remapped.
    nlohmann::json captured;
    {
        SceneSerializer::ScopedRawEntityContext raw(scene);
        captured = meta->serialize(scene.Get<Parent>(b));
    }
    REQUIRE(captured.at("parent").is_number());
    CHECK(captured.at("parent").get<uint32_t>() == a.index);

    // Restore: remove then re-add from the captured JSON under the raw context.
    // (addToScene bottoms out in Scene::Add, which no-ops if the component still
    // exists, so the remove-first is mandatory — mirrors the apply engine.)
    scene.RemoveById(b, meta->id);
    REQUIRE_FALSE(scene.Has<Parent>(b));
    {
        SceneSerializer::ScopedRawEntityContext raw(scene);
        meta->addToScene(&scene, b.index, b.generation, captured);
    }
    const Parent *restored = scene.Get<Parent>(b);
    REQUIRE(restored != nullptr);
    CHECK(restored->parent == a); // the whole point: NOT silently flattened to NullEntity
}

TEST_CASE("SceneSerializer: WITHOUT a context an EntityRef collapses (the bug the scope fixes)")
{
    ECS::Scene scene;
    const ECS::Entity a = scene.Create();
    const ECS::Entity b = scene.Create();
    REQUIRE(scene.Add(b, Parent{.parent = a}) != nullptr);

    const auto *meta = Core::Reflect::ComponentRegistry::Instance().Find("Parent");
    REQUIRE(meta != nullptr);

    // No context engaged: EntityToRef has nothing to address the target by, so a
    // live handle encodes as null and deserializes back to NullEntity — the silent
    // hierarchy flattening that ScopedRawEntityContext exists to prevent.
    const nlohmann::json flat = meta->serialize(scene.Get<Parent>(b));
    CHECK(flat.at("parent").is_null());

    scene.RemoveById(b, meta->id);
    meta->addToScene(&scene, b.index, b.generation, flat);
    const Parent *restored = scene.Get<Parent>(b);
    REQUIRE(restored != nullptr);
    CHECK(restored->parent == ECS::NullEntity);
}

// Under ScopedRawEntityContext, EntityToRef packs (slot, generation) rather than
// the bare slot, and RefToEntity resolves only while the slot's occupant still
// carries that generation. A bare slot would resolve a ref captured to a
// destroyed entity onto whatever entity reused the slot, and no liveness check
// could catch it — the recycled slot is perfectly alive.
// (Only one raw context per thread — non-reentrant — so each phase is scoped.)
TEST_CASE("SceneSerializer: a raw ref to a recycled slot resolves to null, not its new occupant")
{
    ECS::Scene scene;
    const ECS::Entity e = scene.Create();
    REQUIRE(e.generation == 0);

    // Capture: the raw context encodes slot + generation.
    nlohmann::json captured;
    {
        SceneSerializer::ScopedRawEntityContext raw(scene);
        captured = SceneSerializer::EntityToRef(e);
        REQUIRE(captured.is_number_unsigned());
        // Round-trips to the same entity while it is still alive.
        CHECK(SceneSerializer::RefToEntity(captured) == e);
    }

    // Destroy e, then create a fresh entity that reuses the SAME slot with a bumped
    // generation — the classic dangling-handle setup.
    scene.Destroy(e);
    scene.FlushDestroyed();
    const ECS::Entity e2 = scene.Create();
    REQUIRE(e2.index == e.index);           // slot reused
    REQUIRE(e2.generation != e.generation); // but a new generation

    {
        SceneSerializer::ScopedRawEntityContext raw(scene);
        const ECS::Entity resolved = SceneSerializer::RefToEntity(captured);
        CHECK(resolved == ECS::NullEntity); // NOT silently redirected onto e2
        CHECK(resolved != e2);

        // A ref captured to the new occupant still resolves normally.
        const nlohmann::json freshKey = SceneSerializer::EntityToRef(e2);
        REQUIRE(freshKey.is_number_unsigned());
        CHECK(SceneSerializer::RefToEntity(freshKey) == e2);
    }
}

// ---------------------------------------------------------------------------
// Nesting — one serialization context per thread, and an entry point reached
// from inside another one must not silently take it over. Load clears the
// caller's scene, so it refuses; the outer context's tables would otherwise
// name entities the clear destroyed.
// ---------------------------------------------------------------------------

TEST_CASE("SceneSerializer: Load inside a raw-entity context refuses, and the caller keeps its scene")
{
    ECS::Scene source;
    const ECS::Entity written = source.Create();
    REQUIRE(source.Add(written, Transform{.position = {1.f, 2.f, 3.f}}) != nullptr);
    const nlohmann::json level = SceneSerializer::Save(source);

    ECS::Scene scene;
    const ECS::Entity keep = scene.Create();
    REQUIRE(scene.Add(keep, Transform{.position = {9.f, 9.f, 9.f}}) != nullptr);

    const SceneSerializer::ScopedRawEntityContext raw(scene);

    const LevelResult result = SceneSerializer::Load(scene, level);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::ContextBusy);
    // Refused before the clear, so the caller's handles are still good.
    CHECK_FALSE(result.error().sceneReplaced);

    const Transform *kept = scene.Get<Transform>(keep);
    REQUIRE(kept != nullptr);
    CHECK(kept->position.x == doctest::Approx(9.f));
}

// The three below engage a context directly, which only the serializer's own
// translation units can do — hence the private header. Nothing else in the tree
// includes it; a test is the one other place that has business proving what the
// guard does.

TEST_CASE("SceneSerializer: a Save nested inside another context hands it back untouched")
{
    ECS::Scene outerScene;
    const ECS::Entity target = outerScene.Create();

    ECS::Scene toSave;
    const ECS::Entity written = toSave.Create();
    REQUIRE(toSave.Add(written, Transform{}) != nullptr);

    Runtime::SerializationContext ctx;
    ctx.entityToName.emplace(Runtime::EntityKey(target.index, target.generation), "outer_target");
    const Runtime::ScopedContext outer(std::move(ctx));

    const nlohmann::json before = SceneSerializer::EntityToRef(target);
    REQUIRE(before.is_string());
    REQUIRE(before.get<std::string>() == "outer_target");

    // A save only reads its scene, so nesting one is legal — and it must leave the
    // context it ran under exactly as it found it.
    const nlohmann::json level = SceneSerializer::Save(toSave);
    CHECK(level.at("entities").size() == 1);

    const nlohmann::json after = SceneSerializer::EntityToRef(target);
    REQUIRE(after.is_string()); // was null once: Save blanked the outer context
    CHECK(after.get<std::string>() == "outer_target");
}

TEST_CASE("SceneSerializer: Load inside another serialization context refuses and leaves it live")
{
    ECS::Scene source;
    const ECS::Entity written = source.Create();
    REQUIRE(source.Add(written, Transform{}) != nullptr);
    const nlohmann::json level = SceneSerializer::Save(source);

    ECS::Scene scene;
    const ECS::Entity target = scene.Create();
    REQUIRE(scene.Add(target, Transform{.position = {4.f, 4.f, 4.f}}) != nullptr);

    Runtime::SerializationContext ctx;
    ctx.entityToName.emplace(Runtime::EntityKey(target.index, target.generation), "outer_target");
    const Runtime::ScopedContext outer(std::move(ctx));

    const LevelResult result = SceneSerializer::Load(scene, level);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == LevelError::ContextBusy);
    CHECK_FALSE(result.error().sceneReplaced);

    // The scene the outer context speaks for is still standing.
    const Transform *kept = scene.Get<Transform>(target);
    REQUIRE(kept != nullptr);
    CHECK(kept->position.x == doctest::Approx(4.f));

    const nlohmann::json after = SceneSerializer::EntityToRef(target);
    REQUIRE(after.is_string());
    CHECK(after.get<std::string>() == "outer_target");
}

TEST_CASE("SceneSerializer: nested contexts restore in order, and the outermost leaves none behind")
{
    ECS::Scene scene;
    const ECS::Entity e    = scene.Create();
    const uint64_t key = Runtime::EntityKey(e.index, e.generation);

    // Which context is live, read the way the generated code reads it.
    const auto liveName = [&]
                          {
                              const nlohmann::json ref = SceneSerializer::EntityToRef(e);
                              return ref.is_string() ? ref.get<std::string>() : std::string{};
                          };

    const auto engage = [&key](std::string name)
                        {
                            Runtime::SerializationContext ctx;
                            ctx.entityToName.emplace(key, std::move(name));
                            return ctx;
                        };

    // An entry point that gives up part-way: the exit path that is not falling off
    // the end of the block, which is how most of the refusals in these files leave.
    const auto nestThenReturnEarly = [&](std::string name)
                                     {
                                         const Runtime::ScopedContext scoped(engage(std::move(name)));
                                         if (liveName().empty())
                                             return std::string{}; // unreachable; the guard just engaged one
                                         return liveName();
                                     };

    CHECK(liveName().empty()); // nothing engaged

    {
        const Runtime::ScopedContext outer(engage("outer"));
        CHECK(liveName() == "outer");
        {
            const Runtime::ScopedContext middle(engage("middle"));
            CHECK(liveName() == "middle");
            {
                const Runtime::ScopedContext inner(engage("inner"));
                CHECK(liveName() == "inner");
            }
            // Three deep, because two cannot tell "restores the previous one" from
            // "restores the outermost one".
            CHECK(liveName() == "middle");
        }
        CHECK(liveName() == "outer");

        CHECK(nestThenReturnEarly("transient") == "transient");
        CHECK(liveName() == "outer");
    }

    // Past the outermost entry point the hooks must see nothing again — a guard
    // that restored its own table here would leak a context into whatever runs next.
    CHECK(liveName().empty());
}

// A single non-finite float must not make the whole level file unloadable.
// Unsanitized, NaN dumps as JSON null, get<float>() on null throws, and
// LoadFromFile catches it and Clear()s -> the entire scene loads empty. Save
// replaces non-finite values with 0 instead (SanitizeNonFinite).
// (Only reproduces through the file path: in-memory Load carries NaN as a
// number. This mirrors the real autosave-then-reload scenario.)
TEST_CASE("SceneSerializer: one non-finite float does not brick the whole level file")
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "assisi_serializer_nan_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());

    ECS::Scene scene;
    const ECS::Entity good = scene.Create(); // {0,0}
    REQUIRE(scene.Add(good, Transform{.position = {1.f, 2.f, 3.f}}) != nullptr);
    const ECS::Entity bad = scene.Create(); // {1,0}
    REQUIRE(scene.Add(bad, Transform{.position = {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f}}) != nullptr);

    REQUIRE(SceneSerializer::SaveToFile(scene, root / "nan.alvl"));

    ECS::Scene loaded;
    const bool ok = SceneSerializer::LoadFromFile(loaded, "nan.alvl").has_value();
    CHECK(ok); // the load must not fail wholesale
    // The well-formed entity must survive — one bad float can't empty the scene.
    CHECK(loaded.Get<Transform>(ECS::Entity{.index = 0, .generation = 0}) != nullptr);

    fs::remove_all(root, ec);
}
