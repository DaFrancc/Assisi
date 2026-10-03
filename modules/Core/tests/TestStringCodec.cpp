/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestStringCodec.cpp
/// @brief The string types through the binary codec and the container JSON: every
/// one round-trips, an InternedString travels as its text rather than its index,
/// each type has its own protocol name, and a hostile pool size is refused before
/// anything is allocated.
///
/// The metas are hand-built, as TestContainerCodec's are, so this fails when the
/// codec breaks rather than when some engine struct changes shape.

#include <doctest/doctest.h>

#include <ostream>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/DisplayedString.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/ContainerJson.hpp>
#include <Assisi/Core/Reflect/ContainerOps.hpp>
#include <Assisi/Core/StringPool.hpp>

using Assisi::Core::BitReader;
using Assisi::Core::BitWriter;
using Assisi::Core::DisplayedString;
using Assisi::Core::InternedString;
using Assisi::Core::PooledString;
using Assisi::Core::StringPool;
using Assisi::Core::Reflect::AssetLayoutHash;
using Assisi::Core::Reflect::AssetTypeMeta;
using Assisi::Core::Reflect::ComponentId;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::ContainerSpecFor;
using Assisi::Core::Reflect::ContainerToJson;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Core::Reflect::kAllFields;
using Assisi::Core::Reflect::ReadAsset;
using Assisi::Core::Reflect::ReadComponent;
using Assisi::Core::Reflect::ReadComponentId;
using Assisi::Core::Reflect::ReadContainer;
using Assisi::Core::Reflect::WriteAsset;
using Assisi::Core::Reflect::WriteComponent;

namespace
{

/// One of each string type, and a list of each that may be one.
struct Texts
{
    StringPool pool;
    std::vector<PooledString> rows;
    std::vector<InternedString> tags;
    DisplayedString label;
    InternedString style;
    PooledString title;

    bool operator==(const Texts &) const = default;
};

template <typename T, typename M> std::size_t OffsetOf(M T::*member)
{
    static const T prototype{};
    return static_cast<std::size_t>(reinterpret_cast<const std::byte *>(&(prototype.*member)) -
                                    reinterpret_cast<const std::byte *>(&prototype));
}

FieldMeta Field(const char *name, FieldType type, std::size_t offset)
{
    FieldMeta field;
    field.name   = name;
    field.type   = type;
    field.offset = offset;
    return field;
}

template <typename C> FieldMeta ContainerField(const char *name, std::size_t offset)
{
    FieldMeta field = Field(name, FieldType::Vector, offset);
    field.container = ContainerSpecFor<C>();
    return field;
}

std::vector<FieldMeta> TextsFields()
{
    return {Field("pool", FieldType::StringPool, OffsetOf(&Texts::pool)),
            ContainerField<std::vector<PooledString>>("rows", OffsetOf(&Texts::rows)),
            ContainerField<std::vector<InternedString>>("tags", OffsetOf(&Texts::tags)),
            Field("label", FieldType::DisplayedString, OffsetOf(&Texts::label)),
            Field("style", FieldType::InternedString, OffsetOf(&Texts::style)),
            Field("title", FieldType::PooledString, OffsetOf(&Texts::title))};
}

ComponentMeta MakeTextsComponent()
{
    ComponentMeta meta{.name = "Texts",
                       .fields = TextsFields(),
                       .typeIndex = std::type_index(typeid(Texts)),
                       .id = ComponentId{12},
                       .serializable = true,
                       .tracksChanges = true,
                       .replicable = true};
    return meta;
}

AssetTypeMeta MakeTextsAsset()
{
    return AssetTypeMeta{.name        = "Texts",
                         .typeIndex   = std::type_index(typeid(Texts)),
                         .fields      = TextsFields(),
                         .serialize   = {},
                         .deserialize = {},
                         .construct   = []() -> void * { return new Texts{}; },
                         .destroy     = [](void *instance) { delete static_cast<Texts *>(instance); }};
}

Texts MakeFilled()
{
    Texts texts;
    texts.title = texts.pool.Add("Paused");
    texts.rows  = {texts.pool.Add("Resume"), texts.pool.Add(""), texts.pool.Add("Quit to menu")};
    texts.tags  = {InternedString{"menu"}, InternedString{}, InternedString{"pause"}};
    texts.label = DisplayedString::FromSource("#pause:title");
    texts.style = InternedString{"heading"};
    return texts;
}

} // namespace

TEST_CASE("StringCodec: every string type round-trips through a component block")
{
    const ComponentMeta meta = MakeTextsComponent();
    const Texts written      = MakeFilled();

    BitWriter writer;
    REQUIRE(WriteComponent(meta, &written, writer, kAllFields));
    BitReader reader(writer.Data());
    REQUIRE(ReadComponentId(reader) == meta.id);

    Texts read;
    REQUIRE(ReadComponent(meta, &read, reader));
    REQUIRE(reader.Ok());
    CHECK(read == written);
    CHECK(read.pool.View(read.title) == "Paused");
    CHECK(read.pool.View(read.rows[2]) == "Quit to menu");
}

TEST_CASE("StringCodec: every string type round-trips through an asset block")
{
    const AssetTypeMeta meta = MakeTextsAsset();
    const Texts written      = MakeFilled();

    BitWriter writer;
    REQUIRE(WriteAsset(meta, &written, writer));
    BitReader reader(writer.Data());

    Texts read;
    REQUIRE(ReadAsset(meta, &read, reader));
    CHECK(reader.Ok());
    CHECK(read == written);
}

TEST_CASE("StringCodec: an InternedString is written as its text, not its index")
{
    AssetTypeMeta meta = MakeTextsAsset();
    meta.fields        = {Field("style", FieldType::InternedString, OffsetOf(&Texts::style))};

    Texts texts;
    texts.style = InternedString{"a-name-only-this-test-interns"};

    BitWriter writer;
    REQUIRE(WriteAsset(meta, &texts, writer));
    const std::span<const std::byte> bytes = writer.Data();
    const std::string_view wire{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    CHECK(wire.find("a-name-only-this-test-interns") != std::string_view::npos);
}

TEST_CASE("StringCodec: each string type has its own protocol name")
{
    // Retyping a field between two string types must move the layout hash, or a
    // reader would decode one type's bytes as another's.
    const std::vector<FieldType> types = {FieldType::String, FieldType::EntityName, FieldType::InternedString,
                                          FieldType::DisplayedString, FieldType::PooledString,
                                          FieldType::StringPool};
    std::vector<std::uint64_t> hashes;
    for (const FieldType type : types)
    {
        AssetTypeMeta meta = MakeTextsAsset();
        meta.fields        = {Field("text", type, 0)};
        hashes.push_back(AssetLayoutHash(meta));
    }
    std::sort(hashes.begin(), hashes.end());
    CHECK(std::adjacent_find(hashes.begin(), hashes.end()) == hashes.end());
}

TEST_CASE("StringCodec: a pool size larger than the bytes present is refused")
{
    AssetTypeMeta meta = MakeTextsAsset();
    meta.fields        = {Field("pool", FieldType::StringPool, OffsetOf(&Texts::pool))};

    // A real block with a short pool, whose size prefix is then swapped for one
    // far beyond the bytes that follow.
    BitWriter header;
    header.WriteUInt64(AssetLayoutHash(meta));
    header.WriteVarUInt64(Assisi::Core::kMaxPoolBytes);
    header.WriteBytes(std::as_bytes(std::span{"abc", 3}));

    Texts read;
    BitReader reader(header.Data());
    CHECK_FALSE(ReadAsset(meta, &read, reader));
    CHECK(reader.Failed());
    CHECK(read.pool.Bytes().empty());
}

TEST_CASE("StringCodec: a list of InternedStrings is JSON text, and reads back")
{
    const std::vector<InternedString> tags = {InternedString{"menu"}, InternedString{"pause"}};
    const nlohmann::json json              = ContainerToJson(tags);
    CHECK(json == nlohmann::json::array({"menu", "pause"}));

    std::vector<InternedString> read;
    REQUIRE(ReadContainer(nlohmann::json{{"tags", json}}, "Texts", "tags", read));
    CHECK(read == tags);
}

TEST_CASE("StringCodec: a list of PooledStrings is JSON offsets and lengths, and reads back")
{
    StringPool pool;
    const std::vector<PooledString> rows = {pool.Add("one"), pool.Add("three")};
    const nlohmann::json json            = ContainerToJson(rows);
    CHECK(json.at(1).at("offset") == 3);
    CHECK(json.at(1).at("length") == 5);

    std::vector<PooledString> read;
    REQUIRE(ReadContainer(nlohmann::json{{"rows", json}}, "Texts", "rows", read));
    CHECK(read == rows);
}

TEST_CASE("StringCodec: a PooledString missing its length is refused in JSON")
{
    std::vector<PooledString> read;
    const nlohmann::json json = nlohmann::json{{"rows", nlohmann::json::array({{{"offset", 1}}})}};
    CHECK_FALSE(ReadContainer(json, "Texts", "rows", read));
}
