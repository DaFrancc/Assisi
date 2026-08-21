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
    m.BaseWeight = 0.8f;
    m.SpecularWeight = 0.7f;
    m.SpecularColor = {0.9f, 0.8f, 0.6f};
    m.SpecularIor = 1.33f;
    m.BaseDiffuseRoughness = 0.4f;
    m.SpecularAntiAliasing = false;
    m.SpecularAaVarianceClamp = 0.35f;
    m.Alpha = Assisi::Geometry::AlphaMode::Mask;
    m.AlphaCutoff = 0.25f;
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
    CHECK(restored->BaseWeight == doctest::Approx(0.8f));
    CHECK(restored->SpecularWeight == doctest::Approx(0.7f));
    CHECK(restored->SpecularColor.z == doctest::Approx(0.6f));
    CHECK(restored->SpecularIor == doctest::Approx(1.33f));
    CHECK(restored->BaseDiffuseRoughness == doctest::Approx(0.4f));
    // The enable is the one field whose default is true, so a round-trip that
    // dropped it would restore the opposite of what was saved.
    CHECK(restored->SpecularAntiAliasing == false);
    CHECK(restored->SpecularAaVarianceClamp == doctest::Approx(0.35f));
    // The enum rides the wire as its underlying integer, so a round-trip that
    // lost it would restore Opaque and the material would stop being a cutout.
    CHECK(restored->Alpha == Assisi::Geometry::AlphaMode::Mask);
    CHECK(restored->AlphaCutoff == doctest::Approx(0.25f));
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
    // Defaults preserved for everything absent. The OpenPBR fields are the
    // live case: every .amat written before they existed omits them, and the
    // defaults are chosen to reproduce the pre-OpenPBR shading exactly
    // (specular_ior 1.5 -> F0 0.04).
    CHECK(restored->RoughnessFactor == doctest::Approx(1.0f));
    CHECK(restored->BaseColorFactor.x == doctest::Approx(1.0f));
    CHECK(restored->BaseColorTexture.IsNil());
    CHECK(restored->BaseWeight == doctest::Approx(1.0f));
    CHECK(restored->SpecularWeight == doctest::Approx(1.0f));
    CHECK(restored->SpecularColor.x == doctest::Approx(1.0f));
    CHECK(restored->SpecularColor.z == doctest::Approx(1.0f));
    CHECK(restored->SpecularIor == doctest::Approx(1.5f));
    CHECK(restored->BaseDiffuseRoughness == doctest::Approx(0.0f));
    // Specular AA is the opposite case: its default is *on*, so an .amat written
    // before it existed gains the filter on load rather than keeping the
    // sparkling it was authored against.
    CHECK(restored->SpecularAntiAliasing == true);
    CHECK(restored->SpecularAaVarianceClamp == doctest::Approx(0.2f));
    // Every .amat committed today predates the alpha fields. Opaque is what the
    // renderer treated them as, so loading them as anything else would change
    // how existing content draws.
    CHECK(restored->Alpha == Assisi::Geometry::AlphaMode::Opaque);
    CHECK(restored->AlphaCutoff == doctest::Approx(0.5f));
}

TEST_CASE("MaterialFile: an .amat written before the alpha fields existed still loads")
{
    // The exact envelope + key set an .amat carried before this change, verbatim.
    // It is the whole non-breaking claim: the schema addition is per-field, so
    // the fields the file does have must land unchanged and the two it lacks must
    // take defaults rather than failing the parse.
    const std::string legacy = R"({
  "version": 1,
  "type": "MaterialData",
  "BaseColorFactor": [0.8, 0.7, 0.6, 1.0],
  "MetallicFactor": 0.0,
  "RoughnessFactor": 0.6,
  "NormalScale": 1.0,
  "OcclusionStrength": 1.0,
  "EmissiveFactor": [0.0, 0.0, 0.0],
  "BaseWeight": 1.0,
  "SpecularWeight": 1.0,
  "SpecularColor": [1.0, 1.0, 1.0],
  "SpecularIor": 1.5,
  "BaseDiffuseRoughness": 0.0,
  "SpecularAntiAliasing": true,
  "SpecularAaVarianceClamp": 0.2,
  "BaseColorTexture": "aaaaaaaa-0000-4000-8000-00000000000a"
})";

    const auto restored = DeserializeMaterial(legacy);
    REQUIRE(restored.has_value());
    CHECK(restored->BaseColorFactor.x == doctest::Approx(0.8f));
    CHECK(restored->RoughnessFactor == doctest::Approx(0.6f));
    CHECK(restored->BaseColorTexture == *Assisi::Core::AssetId::Parse("aaaaaaaa-0000-4000-8000-00000000000a"));
    CHECK(restored->Alpha == Assisi::Geometry::AlphaMode::Opaque);
    CHECK(restored->AlphaCutoff == doctest::Approx(0.5f));
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
