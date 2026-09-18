/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestCookedPayload.cpp
/// @brief The shader, verbatim and reflected blobs read back as what was written,
/// and each refuses bytes that are another kind, cut short, or another type.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <typeindex>
#include <vector>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/CookedPayload.hpp>
#include <Assisi/Core/Reflect/AssetTypeMeta.hpp>

using namespace Assisi::Core;

namespace
{

/// A reflected asset with values no default holds.
struct Settings
{
    float gain   = 1.f;
    bool enabled = false;
};

Reflect::FieldMeta Field(const char *name, Reflect::FieldType type, std::size_t offset)
{
    Reflect::FieldMeta field;
    field.name   = name;
    field.type   = type;
    field.offset = offset;
    return field;
}

Reflect::AssetTypeMeta MakeSettingsMeta(const char *name)
{
    Reflect::AssetTypeMeta meta{.name        = name,
                                .typeIndex   = std::type_index(typeid(Settings)),
                                .fields      = {},
                                .serialize   = {},
                                .deserialize = {},
                                .construct   = [] () -> void * { return new Settings{}; },
                                .destroy     = [] (void *instance) { delete static_cast<Settings *>(instance); }};
    meta.fields.push_back(Field("gain", Reflect::FieldType::Float, offsetof(Settings, gain)));
    meta.fields.push_back(Field("enabled", Reflect::FieldType::Bool, offsetof(Settings, enabled)));
    return meta;
}

std::vector<std::byte> ToVector(std::span<const std::byte> bytes)
{
    return {bytes.begin(), bytes.end()};
}

const std::vector<std::byte> kPayload{std::byte{0x03}, std::byte{0x02}, std::byte{0x23}, std::byte{0x07},
                                      std::byte{0x00}, std::byte{0xFF}};

} // namespace

TEST_CASE("A shader blob reads back as the SPIR-V that was written")
{
    BitWriter writer;
    WriteShaderBlob(writer, kPayload);

    const std::expected<std::vector<std::byte>, CookedPayloadError> read = ReadShaderBlob(writer.Data());
    REQUIRE(read.has_value());
    CHECK(*read == kPayload);
}

TEST_CASE("A verbatim blob reads back as the bytes that were written")
{
    BitWriter writer;
    WriteVerbatimBlob(writer, kPayload);

    const std::expected<std::vector<std::byte>, CookedPayloadError> read = ReadVerbatimBlob(writer.Data());
    REQUIRE(read.has_value());
    CHECK(*read == kPayload);
}

TEST_CASE("A shader blob is not read as verbatim bytes, nor the other way round")
{
    // Both are a length and bytes, so only the header tells them apart. Without
    // the kind check a font handed to the shader path would reach the driver.
    BitWriter shader;
    WriteShaderBlob(shader, kPayload);
    const std::expected<std::vector<std::byte>, CookedPayloadError> asVerbatim = ReadVerbatimBlob(shader.Data());
    REQUIRE_FALSE(asVerbatim.has_value());
    CHECK(asVerbatim.error() == CookedPayloadError::WrongKind);

    BitWriter verbatim;
    WriteVerbatimBlob(verbatim, kPayload);
    const std::expected<std::vector<std::byte>, CookedPayloadError> asShader = ReadShaderBlob(verbatim.Data());
    REQUIRE_FALSE(asShader.has_value());
    CHECK(asShader.error() == CookedPayloadError::WrongKind);
}

TEST_CASE("A truncated byte blob is refused rather than half-read")
{
    BitWriter writer;
    WriteShaderBlob(writer, kPayload);
    const std::vector<std::byte> bytes = ToVector(writer.Data());

    for (std::size_t length = 0; length < bytes.size(); ++length)
    {
        CAPTURE(length);
        CHECK_FALSE(ReadShaderBlob(std::span<const std::byte>{bytes.data(), length}).has_value());
    }
}

TEST_CASE("A reflected blob reads back into an instance of its type")
{
    const Reflect::AssetTypeMeta meta = MakeSettingsMeta("Settings");

    Settings written;
    written.gain    = 0.375f;
    written.enabled = true;

    BitWriter writer;
    REQUIRE(WriteReflectedBlob(writer, meta, &written));

    Settings read;
    REQUIRE(ReadReflectedBlob(writer.Data(), meta, &read).has_value());
    CHECK(read.gain == written.gain);
    CHECK(read.enabled == written.enabled);
}

TEST_CASE("A reflected blob of another type is refused")
{
    // A config file handed to the material path would otherwise decode the
    // config's bits into the material's fields.
    const Reflect::AssetTypeMeta settings = MakeSettingsMeta("Settings");
    const Reflect::AssetTypeMeta other    = MakeSettingsMeta("OtherSettings");

    Settings written;
    BitWriter writer;
    REQUIRE(WriteReflectedBlob(writer, settings, &written));

    Settings read;
    const std::expected<void, CookedPayloadError> result = ReadReflectedBlob(writer.Data(), other, &read);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == CookedPayloadError::WrongType);
}

TEST_CASE("A truncated reflected blob is refused")
{
    const Reflect::AssetTypeMeta meta = MakeSettingsMeta("Settings");
    Settings written;
    BitWriter writer;
    REQUIRE(WriteReflectedBlob(writer, meta, &written));
    const std::vector<std::byte> bytes = ToVector(writer.Data());

    for (std::size_t length = 0; length < bytes.size(); ++length)
    {
        CAPTURE(length);
        Settings read;
        CHECK_FALSE(ReadReflectedBlob(std::span<const std::byte>{bytes.data(), length}, meta, &read).has_value());
    }
}
