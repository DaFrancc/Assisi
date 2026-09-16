/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAssetCodec.cpp
/// @brief The binary form of a reflected asset type (AASSET): a hand-built
/// AssetTypeMeta round-trips every value, a transient field never reaches the
/// blob, and a block written against a different field table is refused rather
/// than decoded into the wrong fields.
///
/// The meta is built by hand for the reason TestBinaryCodec's is: this suite has
/// to fail when the *codec* breaks, not when MaterialData gains a field — and
/// Core links neither Geometry nor glm, so the vector-typed fields are the plain
/// float arrays the codec actually sees.
///
/// The asset path is deliberately not the component path, and the two
/// differences are what most of these cases pin: there is no field mask, and the
/// field filter is `transient` alone rather than `transient || norep`.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <typeindex>
#include <vector>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/ShortString.hpp>

using Assisi::Core::AssetId;
using Assisi::Core::AssetPath;
using Assisi::Core::BitReader;
using Assisi::Core::BitWriter;
using Assisi::Core::ShortString;
using Assisi::Core::Reflect::AssetLayoutDescription;
using Assisi::Core::Reflect::AssetLayoutHash;
using Assisi::Core::Reflect::AssetTypeMeta;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Core::Reflect::IsAssetField;
using Assisi::Core::Reflect::ReadAsset;
using Assisi::Core::Reflect::WriteAsset;

namespace
{

/// A stand-in for a `.amat`: the field shapes a material actually carries —
/// factors, a name, a texture id, a list of ids — plus the two fields whose
/// treatment differs from the wire's.
struct Material
{
    std::array<float, 4> baseColorFactor{1.f, 1.f, 1.f, 1.f};
    std::array<float, 3> emissiveFactor{};
    float metallicFactor  = 1.f;
    float roughnessFactor = 1.f;
    bool doubleSided      = false;

    ShortString name;
    AssetPath sourcePath;
    AssetId baseColorTexture;
    std::vector<AssetId> layers;

    /// Saved to disk, never sent over the network. The wire filter drops this;
    /// the asset filter must not, because here the destination *is* disk.
    std::int32_t editorRevision = 0;

    /// A resolved GPU handle: rebuilt on load, meaningless in a file.
    float cachedLod = 0.f;

    bool operator==(const Material &) const = default;
};

/// Byte offset without offsetof — Material holds a std::vector, so it is not a
/// standard-layout type. Same trick TestBinaryCodec uses, and the same thing
/// reflectgen's offsetof resolves to.
template <typename T, typename M> std::size_t OffsetOf(M T::*member)
{
    static const T prototype{};
    return static_cast<std::size_t>(reinterpret_cast<const std::byte *>(&(prototype.*member)) -
                                    reinterpret_cast<const std::byte *>(&prototype));
}

FieldMeta Field(const char *name, FieldType type, std::size_t offset, bool transient = false, bool norep = false)
{
    FieldMeta field;
    field.name      = name;
    field.type      = type;
    field.offset    = offset;
    field.transient = transient;
    field.norep     = norep;
    return field;
}

/// The descriptor reflectgen would emit for Material. Built per call so a test
/// that mutates it cannot leak into the next.
AssetTypeMeta MakeMaterialMeta()
{
    AssetTypeMeta meta{.name        = "Material",
                       .typeIndex   = std::type_index(typeid(Material)),
                       .fields      = {},
                       .serialize   = {},
                       .deserialize = {},
                       .construct   = [] () -> void * { return new Material{}; },
                       .destroy     = [] (void *instance) { delete static_cast<Material *>(instance); }};

    meta.fields.push_back(Field("baseColorFactor", FieldType::Vec4, OffsetOf(&Material::baseColorFactor)));
    meta.fields.push_back(Field("emissiveFactor", FieldType::Vec3, OffsetOf(&Material::emissiveFactor)));
    meta.fields.push_back(Field("metallicFactor", FieldType::Float, OffsetOf(&Material::metallicFactor)));
    meta.fields.push_back(Field("roughnessFactor", FieldType::Float, OffsetOf(&Material::roughnessFactor)));
    meta.fields.push_back(Field("doubleSided", FieldType::Bool, OffsetOf(&Material::doubleSided)));
    meta.fields.push_back(Field("name", FieldType::String, OffsetOf(&Material::name)));
    meta.fields.push_back(Field("sourcePath", FieldType::AssetPath, OffsetOf(&Material::sourcePath)));
    meta.fields.push_back(Field("baseColorTexture", FieldType::AssetId, OffsetOf(&Material::baseColorTexture)));
    meta.fields.push_back(Field("layers", FieldType::AssetIdVector, OffsetOf(&Material::layers)));
    meta.fields.push_back(
        Field("editorRevision", FieldType::Int32, OffsetOf(&Material::editorRevision), false, /*norep=*/ true));
    meta.fields.push_back(Field("cachedLod", FieldType::Float, OffsetOf(&Material::cachedLod), /*transient=*/ true));
    return meta;
}

/// Every field set to something no default constructor produces, so a field the
/// codec silently skipped comes back as its default and the comparison fails.
Material MakeFilledMaterial()
{
    Material material;
    material.baseColorFactor = {0.25f, 0.5f, 0.75f, 0.125f};
    material.emissiveFactor  = {2.f, 3.f, 4.f};
    material.metallicFactor  = 0.375f;
    material.roughnessFactor = 0.625f;
    material.doubleSided     = true;
    material.name            = ShortString{"brushed steel"};
    material.sourcePath      = AssetPath{"materials/steel.amat"};
    material.baseColorTexture =
        *AssetId::Parse("3f2504e0-4f89-41d3-9a0c-0305e82c3301");
    material.layers = {*AssetId::Parse("0b5f1d2c-3e4a-4b6c-8d7e-9f0a1b2c3d4e"),
                       *AssetId::Parse("11111111-2222-4333-8444-555555555555")};
    material.editorRevision = 17;
    material.cachedLod      = 9.f;
    return material;
}

} // namespace

TEST_CASE("Every non-transient field of an asset survives the round trip")
{
    const AssetTypeMeta meta = MakeMaterialMeta();
    const Material written   = MakeFilledMaterial();

    BitWriter writer;
    REQUIRE(WriteAsset(meta, &written, writer));

    Material read;
    BitReader reader{writer.Data()};
    REQUIRE(ReadAsset(meta, &read, reader));
    CHECK_FALSE(reader.Failed());

    CHECK(read.baseColorFactor == written.baseColorFactor);
    CHECK(read.emissiveFactor == written.emissiveFactor);
    CHECK(read.metallicFactor == written.metallicFactor);
    CHECK(read.roughnessFactor == written.roughnessFactor);
    CHECK(read.doubleSided == written.doubleSided);
    CHECK(read.name == written.name);
    CHECK(read.sourcePath == written.sourcePath);
    CHECK(read.baseColorTexture == written.baseColorTexture);
    CHECK(read.layers == written.layers);
}

TEST_CASE("A norep field reaches a cooked asset, unlike the wire")
{
    // The one case that would be silently lost if the asset codec reused
    // IsWireField: norep means "to disk but never to the network", and this
    // destination is disk.
    const AssetTypeMeta meta = MakeMaterialMeta();
    const Material written   = MakeFilledMaterial();

    BitWriter writer;
    REQUIRE(WriteAsset(meta, &written, writer));

    Material read;
    BitReader reader{writer.Data()};
    REQUIRE(ReadAsset(meta, &read, reader));

    CHECK(read.editorRevision == 17);
}

TEST_CASE("A transient field is absent from a cooked asset")
{
    const AssetTypeMeta meta = MakeMaterialMeta();
    const Material written   = MakeFilledMaterial();

    BitWriter writer;
    REQUIRE(WriteAsset(meta, &written, writer));

    Material read;
    read.cachedLod = 4.f; // a value neither the default nor the one written
    BitReader reader{writer.Data()};
    REQUIRE(ReadAsset(meta, &read, reader));

    // Untouched: the field never entered the blob, so the decode cannot have
    // overwritten it with the 9.f the source held.
    CHECK(read.cachedLod == 4.f);
}

TEST_CASE("A block written against a different field table is refused")
{
    const AssetTypeMeta writeMeta = MakeMaterialMeta();
    const Material written        = MakeFilledMaterial();

    BitWriter writer;
    REQUIRE(WriteAsset(writeMeta, &written, writer));

    // A rename, so the encoded width does not move by a single bit. That is what
    // makes this a test of the layout guard rather than of the reader running
    // out of buffer: without the guard this block decodes cleanly and silently
    // into a field table that is no longer the one it was written for.
    AssetTypeMeta readMeta = MakeMaterialMeta();
    readMeta.fields[2].name = "specularFactor";

    Material read;
    BitReader reader{writer.Data()};
    CHECK_FALSE(ReadAsset(readMeta, &read, reader));
    // Nothing was consumed past the hash, so the refusal is the guard's and not
    // an overrun's.
    CHECK_FALSE(reader.Failed());
}

TEST_CASE("The layout hash moves for a change that alters the bits, and not otherwise")
{
    const AssetTypeMeta base = MakeMaterialMeta();
    const std::uint64_t hash = AssetLayoutHash(base);

    SUBCASE("renaming a field moves it")
    {
        AssetTypeMeta renamed     = MakeMaterialMeta();
        renamed.fields[0].name    = "albedoFactor";
        CHECK(AssetLayoutHash(renamed) != hash);
    }

    SUBCASE("retyping a field moves it")
    {
        AssetTypeMeta retyped     = MakeMaterialMeta();
        retyped.fields[2].type    = FieldType::Double;
        CHECK(AssetLayoutHash(retyped) != hash);
    }

    SUBCASE("making a field transient moves it, because it leaves the blob")
    {
        AssetTypeMeta dropped         = MakeMaterialMeta();
        dropped.fields[2].transient   = true;
        CHECK(AssetLayoutHash(dropped) != hash);
    }

    SUBCASE("making a field norep does not, because it stays in the blob")
    {
        // The wire's ProtocolHash moves for this; an asset's must not, or every
        // cooked blob is invalidated by a change that does not touch its bytes.
        AssetTypeMeta gated       = MakeMaterialMeta();
        gated.fields[2].norep     = true;
        CHECK(AssetLayoutHash(gated) == hash);
    }

    SUBCASE("the offset is memory layout, not blob layout")
    {
        AssetTypeMeta moved      = MakeMaterialMeta();
        moved.fields[2].offset  += 4u;
        CHECK(AssetLayoutHash(moved) == hash);
    }
}

TEST_CASE("The layout description names the type and its encoded fields")
{
    // Readable on purpose: when a build refuses a blob, diffing two of these is
    // what names the field that moved.
    const std::string text = AssetLayoutDescription(MakeMaterialMeta());

    CHECK(text.find("Material") != std::string::npos);
    CHECK(text.find("baseColorFactor") != std::string::npos);
    CHECK(text.find("editorRevision") != std::string::npos);
    // Transient fields are not encoded, so they are not part of the layout.
    CHECK(text.find("cachedLod") == std::string::npos);
}

TEST_CASE("A truncated block fails rather than returning half a value")
{
    const AssetTypeMeta meta = MakeMaterialMeta();
    const Material written   = MakeFilledMaterial();

    BitWriter writer;
    REQUIRE(WriteAsset(meta, &written, writer));

    const std::span<const std::byte> full = writer.Data();
    const std::span<const std::byte> cut  = full.first(full.size() / 2);

    Material read;
    BitReader reader{cut};
    CHECK_FALSE(ReadAsset(meta, &read, reader));
}

TEST_CASE("IsAssetField keeps norep and drops transient")
{
    const AssetTypeMeta meta = MakeMaterialMeta();

    std::size_t encoded = 0;
    for (const FieldMeta &field : meta.fields)
    {
        if (IsAssetField(field))
        {
            ++encoded;
        }
    }
    // Eleven declared, one transient.
    CHECK(encoded == meta.fields.size() - 1u);
}

TEST_CASE("The construct and destroy hooks own an instance the caller cannot name")
{
    const AssetTypeMeta meta = MakeMaterialMeta();
    REQUIRE(meta.construct);
    REQUIRE(meta.destroy);

    void *instance = meta.construct();
    REQUIRE(instance != nullptr);

    const Material written = MakeFilledMaterial();
    BitWriter writer;
    REQUIRE(WriteAsset(meta, &written, writer));

    BitReader reader{writer.Data()};
    CHECK(ReadAsset(meta, instance, reader));
    CHECK(static_cast<const Material *>(instance)->name == written.name);

    meta.destroy(instance);
}
