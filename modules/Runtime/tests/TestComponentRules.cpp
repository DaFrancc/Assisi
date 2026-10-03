/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestComponentRules.cpp
/// @brief Loading files whose components carry requires/excludes rules: a saved
/// requirement keeps its saved value, and an excluded pair loads with one of the
/// two dropped and the rest intact.

#include <doctest/doctest.h>

#include <ostream>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Runtime/Blueprint.hpp>
#include <Assisi/Runtime/CookedScene.hpp>
#include <Assisi/Runtime/NameComponent.hpp>
#include <Assisi/Runtime/SceneSerializer.hpp>
#include <Assisi/Runtime/Tests/RuleComponents.hpp>

#include "LogCapture.hpp"

using namespace Assisi;
using Assisi::Runtime::InstanceTable;
using Assisi::Runtime::SceneSerializer;
using Assisi::Runtime::Tests::LoadNeeds;
using Assisi::Runtime::Tests::LoadNeedsShuns;
using Assisi::Runtime::Tests::LoadShuns;
using Assisi::Runtime::Tests::LoadState;

namespace
{

/// A value no default produces, so a reset shows.
constexpr int32_t kSaved = 7;

/// A fresh asset root per case, so a cached definition from one cannot answer another.
std::filesystem::path FreshRoot(const std::string &name)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("assisi_rules_" + name);
    std::error_code code;
    std::filesystem::remove_all(root, code);
    std::filesystem::create_directories(root);
    REQUIRE(Core::AssetSystem::SetRoot(root).has_value());
    Runtime::ClearBlueprintCache();
    return root;
}

void Write(const std::filesystem::path &path, const nlohmann::json &document)
{
    std::ofstream out(path, std::ios::binary);
    out << document.dump(2);
    REQUIRE(out.good());
}

nlohmann::json Entity(const std::string &name, nlohmann::json components)
{
    return {{"name", name}, {"components", std::move(components)}};
}

/// One entity holding a requirer and its saved requirement, and one holding an
/// excluded pair next to a component neither rule touches.
nlohmann::json RuleLevel()
{
    return {{"version", 2},
        {"entities", nlohmann::json::array(
             {Entity("holder", {{"LoadNeeds", nlohmann::json::object()}, {"LoadState", {{"value", kSaved}}}}),
              Entity("clash", {{"LoadShuns", nlohmann::json::object()}, {"LoadState", {{"value", kSaved}}},
                         {"Transform", {{"position", {1.f, 2.f, 3.f}}}}})})}};
}

/// The live entity named @p name, or NullEntity.
ECS::Entity Named(ECS::Scene &scene, const std::string &name)
{
    for (auto [entity, entityName] : scene.Query<Runtime::Name>())
    {
        if (entityName.value.View() == name)
        {
            return entity;
        }
    }
    return ECS::NullEntity;
}

} // namespace

TEST_CASE("Loading keeps a saved requirement's value over the default its requirer adds")
{
    ECS::Scene scene;
    REQUIRE(SceneSerializer::Load(scene, RuleLevel()).has_value());

    const ECS::Entity holder = Named(scene, "holder");
    REQUIRE(scene.Has<LoadNeeds>(holder));
    REQUIRE(scene.Get<LoadState>(holder) != nullptr);
    CHECK(scene.Get<LoadState>(holder)->value == kSaved);
}

TEST_CASE("A level with an excluded pair drops one, names the file, and loads the rest")
{
    const std::filesystem::path root = FreshRoot("level");
    const std::filesystem::path path = root / "clash.alvl";
    Write(path, RuleLevel());

    const Tests::LogCapture log;
    ECS::Scene scene;
    REQUIRE(SceneSerializer::LoadFromDisk(scene, path).has_value());

    const ECS::Entity clash = Named(scene, "clash");
    REQUIRE(clash != ECS::NullEntity);
    CHECK(scene.Has<LoadShuns>(clash) != scene.Has<LoadState>(clash));
    CHECK(scene.Has<ECS::Transform>(clash));
    CHECK(log.Mentions("clash.alvl"));
    CHECK(log.Mentions("'clash'"));

    // The other entity is untouched by the clash.
    CHECK(scene.Get<LoadState>(Named(scene, "holder"))->value == kSaved);
}

TEST_CASE("A level drops a component whose requirement is excluded, and loads the rest")
{
    const nlohmann::json level = {{"version", 2},
        {"entities", nlohmann::json::array(
             {Entity("pulled", {{"LoadNeedsShuns", nlohmann::json::object()}, {"LoadState", {{"value", kSaved}}},
                         {"Transform", {{"position", {1.f, 2.f, 3.f}}}}})})}};

    ECS::Scene scene;
    REQUIRE(SceneSerializer::Load(scene, level).has_value());

    // LoadNeedsShuns reads first and brings LoadShuns, so LoadState is the one
    // that cannot follow; either way the two never share the entity.
    const ECS::Entity pulled = Named(scene, "pulled");
    REQUIRE(pulled != ECS::NullEntity);
    CHECK_FALSE((scene.Has<Runtime::Tests::LoadShuns>(pulled) && scene.Has<LoadState>(pulled)));
    CHECK(scene.Has<LoadNeedsShuns>(pulled) == scene.Has<Runtime::Tests::LoadShuns>(pulled));
    CHECK(scene.Has<ECS::Transform>(pulled));
}

TEST_CASE("A blueprint with an excluded pair expands with one dropped")
{
    const std::filesystem::path root = FreshRoot("blueprint");
    Write(root / "clash.abp", RuleLevel());

    const Tests::LogCapture log;
    ECS::Scene scene;
    InstanceTable table;
    REQUIRE(SceneSerializer::ExpandInstance(scene, table, "clash.abp", ECS::Transform{}).has_value());

    bool sawClash = false;
    for (auto [entity, shuns] : scene.Query<LoadShuns>())
    {
        sawClash = true;
        CHECK_FALSE(scene.Has<LoadState>(entity));
        CHECK(scene.Has<ECS::Transform>(entity));
    }
    CHECK(sawClash);
    CHECK(log.Mentions("clash.abp"));
}

TEST_CASE("A cooked requirement keeps its value through the document a cooked scene loads as")
{
    ECS::Scene source;
    const ECS::Entity holder = source.Create();
    (void)source.Add<Runtime::Name>(holder, Runtime::Name{Core::EntityName{"holder"}});
    REQUIRE(source.Add<LoadNeeds>(holder) != nullptr);
    source.GetMut<LoadState>(holder)->value = kSaved;

    const auto bytes = Runtime::SaveCookedScene(source, Runtime::LevelHeader{}, nullptr,
                                                [](std::string_view) { return Core::AssetId{}; });
    REQUIRE(bytes.has_value());
    const auto cooked = Runtime::DecodeCookedScene(*bytes);
    REQUIRE(cooked.has_value());
    const std::expected<nlohmann::json, Runtime::LevelError> document = Runtime::CookedSceneToDocument(*cooked);
    REQUIRE(document.has_value());

    ECS::Scene loaded;
    REQUIRE(SceneSerializer::Load(loaded, *document).has_value());
    const ECS::Entity loadedHolder = Named(loaded, "holder");
    REQUIRE(loaded.Get<LoadState>(loadedHolder) != nullptr);
    CHECK(loaded.Get<LoadState>(loadedHolder)->value == kSaved);
}
