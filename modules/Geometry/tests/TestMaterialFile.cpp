/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <string>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Geometry/MaterialFile.hpp>

using Assisi::Geometry::DeserializeMaterial;
using Assisi::Geometry::MaterialData;
using Assisi::Geometry::MaterialFileError;
using Assisi::Geometry::SerializeMaterial;

namespace
{
// A material with every field set to a non-default value, so a round-trip that
// silently dropped or defaulted any field would be caught.
MaterialData MakeFullMaterial()
{
    MaterialData m;
    m.BaseColorFactor = {0.1f, 0.2f, 0.3f, 0.4f};
    m.MetallicFactor = 0.25f;
    m.RoughnessFactor = 0.6f;
    m.NormalScale = 0.75f;
    m.OcclusionStrength = 0.5f;
    m.EmissiveFactor = {1.0f, 0.5f, 0.25f};
    // Distinct GUIDs per channel so a round-trip that swapped or dropped one is
    // caught.
    m.BaseColorTexture         = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000001");
    m.NormalTexture            = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000002");
    m.MetallicRoughnessTexture = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000003");
    m.OcclusionTexture         = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000004");
    m.EmissiveTexture          = *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-000000000005");
    m.Name = "crate"; // not serialized — deliberately not round-tripped
    return m;
}
} // namespace

TEST_CASE("MaterialFile: a full material survives a serialize -> deserialize round-trip")
{
    const MaterialData original = MakeFullMaterial();

    const auto text = SerializeMaterial(original);
    REQUIRE(text.has_value());

    const auto restored = DeserializeMaterial(*text);
    REQUIRE(restored.has_value());

    CHECK(restored->BaseColorFactor.x == doctest::Approx(0.1f));
    CHECK(restored->BaseColorFactor.w == doctest::Approx(0.4f));
    CHECK(restored->MetallicFactor == doctest::Approx(0.25f));
    CHECK(restored->RoughnessFactor == doctest::Approx(0.6f));
    CHECK(restored->NormalScale == doctest::Approx(0.75f));
    CHECK(restored->OcclusionStrength == doctest::Approx(0.5f));
    CHECK(restored->EmissiveFactor.y == doctest::Approx(0.5f));
    CHECK(restored->BaseColorTexture == original.BaseColorTexture);
    CHECK(restored->NormalTexture == original.NormalTexture);
    CHECK(restored->MetallicRoughnessTexture == original.MetallicRoughnessTexture);
    CHECK(restored->OcclusionTexture == original.OcclusionTexture);
    CHECK(restored->EmissiveTexture == original.EmissiveTexture);
}

TEST_CASE("MaterialFile: the .amat envelope carries version and type")
{
    const auto text = SerializeMaterial(MaterialData{});
    REQUIRE(text.has_value());
    CHECK(text->find("\"version\"") != std::string::npos);
    CHECK(text->find("\"type\"") != std::string::npos);
    CHECK(text->find("MaterialData") != std::string::npos);
}

TEST_CASE("MaterialFile: absent fields keep their default (forward-compatible load)")
{
    // A minimal document with the correct envelope but only one field set. Every
    // other field must retain its MaterialData default. Keys are the C++ field
    // names verbatim (reflectgen uses the field name), hence PascalCase.
    const std::string minimal = R"({ "version": 1, "type": "MaterialData", "MetallicFactor": 0.9 })";

    const auto restored = DeserializeMaterial(minimal);
    REQUIRE(restored.has_value());
    CHECK(restored->MetallicFactor == doctest::Approx(0.9f));
    // Defaults preserved for everything absent.
    CHECK(restored->RoughnessFactor == doctest::Approx(1.0f));
    CHECK(restored->BaseColorFactor.x == doctest::Approx(1.0f));
    CHECK(restored->BaseColorTexture.IsNil());
}

TEST_CASE("MaterialFile: invalid JSON and wrong type are rejected")
{
    const auto bad = DeserializeMaterial("{ not json");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == MaterialFileError::ParseFailed);

    const auto wrong = DeserializeMaterial(R"({ "version": 1, "type": "SomethingElse" })");
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error() == MaterialFileError::WrongType);
}
