/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestNestedFields.cpp
/// @brief A reflected component holding ASTRUCTs — alone, in a list, in an array,
/// and inside another struct — round-trips through the JSON and binary code
/// reflectgen writes for it, and its nested enums and pooled strings survive.

#include <doctest/doctest.h>

#include <ostream>
#include <string_view>

#include <nlohmann/json.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/TestComponents.hpp>

ASSISI_REFLECTED_STRUCT(Assisi::ECS::Line);

using namespace Assisi;

namespace
{

const Core::Reflect::ComponentMeta &PoemMeta()
{
    const Core::Reflect::ComponentMeta *meta = Core::Reflect::ComponentRegistry::Instance().Find("Poem");
    REQUIRE(meta != nullptr);
    return *meta;
}

ECS::Line MakeLine(ECS::Poem &poem, std::string_view text, ECS::Tone tone)
{
    ECS::Line line;
    line.text = poem.pool.Add(text);
    line.marks = {3, 9};
    line.tone = tone;
    line.scratch = 1.f;
    return line;
}

ECS::Poem MakePoem()
{
    ECS::Poem poem;
    poem.title = MakeLine(poem, "Ozymandias", ECS::Tone::Loud);
    poem.epigraphs = {MakeLine(poem, "I met a traveller", ECS::Tone::Plain),
                      MakeLine(poem, "from an antique land", ECS::Tone::Loud)};
    poem.verses = {ECS::Verse{.first = MakeLine(poem, "Two vast", ECS::Tone::Plain),
                              .rest = {MakeLine(poem, "and trunkless", ECS::Tone::Loud)}}};
    return poem;
}

void CheckLine(const ECS::Poem &read, const ECS::Line &line, std::string_view text, ECS::Tone tone)
{
    CHECK(read.pool.View(line.text) == text);
    CHECK(line.marks == std::array<uint8_t, 2>{3, 9});
    CHECK(line.tone == tone);
    // Transient inside a struct, as on a component: carried by neither codec.
    CHECK(line.scratch == 0.f);
}

void CheckPoem(const ECS::Poem &read)
{
    CheckLine(read, read.title, "Ozymandias", ECS::Tone::Loud);
    CheckLine(read, read.epigraphs[1], "from an antique land", ECS::Tone::Loud);
    REQUIRE(read.verses.size() == 1);
    CheckLine(read, read.verses[0].first, "Two vast", ECS::Tone::Plain);
    REQUIRE(read.verses[0].rest.size() == 1);
    CheckLine(read, read.verses[0].rest[0], "and trunkless", ECS::Tone::Loud);
}

} // namespace

TEST_CASE("Nested structs round-trip through a component's generated JSON")
{
    const Core::Reflect::ComponentMeta &meta = PoemMeta();
    const ECS::Poem written = MakePoem();

    const nlohmann::json json = meta.serialize(&written);
    // The file form: each struct an object of its own fields, an array a list.
    CHECK(json.at("title").at("tone") == 4);
    CHECK(json.at("title").at("marks") == nlohmann::json::array({3, 9}));
    CHECK(json.at("epigraphs").size() == 2);
    CHECK(json.at("verses").at(0).at("rest").size() == 1);
    CHECK_FALSE(json.at("title").contains("scratch"));

    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    REQUIRE(meta.addToScene(&scene, entity.index, entity.generation, json));
    const ECS::Poem *read = scene.Get<ECS::Poem>(entity);
    REQUIRE(read != nullptr);
    CheckPoem(*read);
}

TEST_CASE("Nested JSON that is the wrong shape is refused")
{
    const Core::Reflect::ComponentMeta &meta = PoemMeta();
    const ECS::Poem written = MakePoem();
    nlohmann::json json = meta.serialize(&written);
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();

    SUBCASE("an array of the wrong length")
    {
        json["epigraphs"].erase(0);
        CHECK_FALSE(meta.addToScene(&scene, entity.index, entity.generation, json));
    }
    SUBCASE("a struct that is not an object")
    {
        json["title"] = 7;
        CHECK_FALSE(meta.addToScene(&scene, entity.index, entity.generation, json));
    }
}

TEST_CASE("Nested structs round-trip through the binary codec")
{
    const Core::Reflect::ComponentMeta &meta = PoemMeta();
    const ECS::Poem written = MakePoem();

    Core::BitWriter writer;
    REQUIRE(Core::Reflect::WriteComponent(meta, &written, writer));
    Core::BitReader reader(writer.Data());
    REQUIRE(Core::Reflect::ReadComponentId(reader) == meta.id);

    ECS::Poem read;
    REQUIRE(Core::Reflect::ReadComponent(meta, &read, reader));
    REQUIRE(reader.Ok());
    CheckPoem(read);
}

TEST_CASE("A generated struct's table is reached from hand-written code")
{
    const Core::Reflect::StructSpec *line = Core::Reflect::StructTraits<ECS::Line>::Spec();
    REQUIRE(line != nullptr);
    CHECK(line->name == "Line");
    CHECK(line->fields.size() == 4);

    // A field of a nested struct is part of the component's protocol text.
    const std::string text = Core::Reflect::ProtocolLayoutDescription(std::span{&PoemMeta(), 1});
    CHECK(text.find("rest vector<struct<Line>>") != std::string::npos);
    CHECK(text.find("marks array<u8,2>") != std::string::npos);
}
