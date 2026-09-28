/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestStringFields.cpp
/// @brief A reflected component holding each string type round-trips through the
/// JSON and the binary code generated for it.
///
/// Here rather than in Core's suite because the generated JSON adds components to
/// a Scene, and Core does not depend on ECS.

#include <doctest/doctest.h>

#include <ostream>

#include <string_view>

#include <nlohmann/json.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>

using namespace Assisi;

namespace
{

const Core::Reflect::ComponentMeta &CaptionsMeta()
{
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().Find("Captions");
    REQUIRE(meta != nullptr);
    return *meta;
}

ECS::Captions MakeCaptions()
{
    ECS::Captions captions;
    captions.title = captions.pool.Add("Paused");
    captions.rows  = {captions.pool.Add("Resume"), captions.pool.Add("Quit")};
    captions.label = Core::DisplayedString::FromSource("#pause:title");
    captions.style = Core::InternedString{"heading"};
    return captions;
}

void CheckSame(const ECS::Captions &read, const ECS::Captions &written)
{
    CHECK(read.pool == written.pool);
    CHECK(read.rows == written.rows);
    CHECK(read.label == written.label);
    CHECK(read.style == written.style);
    CHECK(read.title == written.title);
    CHECK(read.pool.View(read.title) == "Paused");
    CHECK(read.pool.View(read.rows[1]) == "Quit");
}

} // namespace

TEST_CASE("A reflected component's string fields round-trip through its generated JSON")
{
    const Core::Reflect::ComponentMeta &meta = CaptionsMeta();
    const ECS::Captions written              = MakeCaptions();

    const nlohmann::json json = meta.serialize(&written);
    // The file form: names as text, handles as numbers beside their pool.
    CHECK(json.at("style") == "heading");
    CHECK(json.at("label") == "#pause:title");
    CHECK(json.at("pool") == "PausedResumeQuit");
    CHECK(json.at("title").at("offset") == 0);
    CHECK(json.at("title").at("length") == 6);

    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(meta.addToScene(&scene, entity.index, entity.generation, json));
    const ECS::Captions *read = scene.Get<ECS::Captions>(entity);
    REQUIRE(read != nullptr);
    CheckSame(*read, written);
}

TEST_CASE("A reflected component's string fields round-trip through the binary codec")
{
    const Core::Reflect::ComponentMeta &meta = CaptionsMeta();
    const ECS::Captions written              = MakeCaptions();

    Core::BitWriter writer;
    REQUIRE(Core::Reflect::WriteComponent(meta, &written, writer));
    Core::BitReader reader(writer.Data());
    REQUIRE(Core::Reflect::ReadComponentId(reader) == meta.id);

    ECS::Captions read;
    REQUIRE(Core::Reflect::ReadComponent(meta, &read, reader));
    REQUIRE(reader.Ok());
    CheckSame(read, written);
}
