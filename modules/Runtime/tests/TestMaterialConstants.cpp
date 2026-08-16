/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Render/Material.hpp>

using Assisi::Geometry::MaterialData;
using Assisi::Render::Material;
using Assisi::Render::MaterialTextures;

TEST_CASE("Material: a default MaterialData packs OpenPBR defaults that reproduce the legacy BRDF")
{
    Material material;
    material.Create(nullptr, 0, MaterialData{}, MaterialTextures{});
    const auto &c = material.Constants();

    CHECK(c.specularColorIor.x == 1.0f);
    CHECK(c.specularColorIor.y == 1.0f);
    CHECK(c.specularColorIor.z == 1.0f);
    CHECK(c.specularColorIor.w == 1.5f);
    CHECK(c.openPbrParams.x == 1.0f); // baseWeight
    CHECK(c.openPbrParams.y == 1.0f); // specularWeight
    CHECK(c.openPbrParams.z == 0.0f); // baseDiffuseRoughness

    // BaseDiffuseRoughness 0 must leave the EON diffuse lobe off entirely.
    CHECK((c.flags.x & Assisi::Render::kMaterialFlagEnergyPreservingDiffuse) == 0u);

    // The F0 the shader derives from these lanes must be the 0.04 the BRDF
    // hardcoded before OpenPBR — that identity is what keeps every existing
    // level pixel-identical.
    const float ior = c.specularColorIor.w;
    const float r0 = (ior - 1.0f) / (ior + 1.0f);
    const float f0 = r0 * r0 * c.openPbrParams.y * c.specularColorIor.x;
    CHECK(f0 == doctest::Approx(0.04f).epsilon(1e-6));
}

TEST_CASE("Material: OpenPBR factors land in their lanes")
{
    MaterialData src;
    src.BaseWeight = 0.5f;
    src.SpecularWeight = 0.25f;
    src.SpecularColor = {0.9f, 0.8f, 0.7f};
    src.SpecularIor = 1.1f;
    src.BaseDiffuseRoughness = 0.3f;

    Material material;
    material.Create(nullptr, 0, src, MaterialTextures{});
    const auto &c = material.Constants();

    CHECK(c.specularColorIor.x == 0.9f);
    CHECK(c.specularColorIor.y == 0.8f);
    CHECK(c.specularColorIor.z == 0.7f);
    CHECK(c.specularColorIor.w == 1.1f);
    CHECK(c.openPbrParams.x == 0.5f);
    CHECK(c.openPbrParams.y == 0.25f);
    CHECK(c.openPbrParams.z == 0.3f);

    // A non-zero diffuse roughness is what turns the EON lobe on.
    CHECK((c.flags.x & Assisi::Render::kMaterialFlagEnergyPreservingDiffuse) != 0u);
}

TEST_CASE("Material: the legacy lanes are untouched by the OpenPBR additions")
{
    MaterialData src;
    src.BaseColorFactor = {0.1f, 0.2f, 0.3f, 0.4f};
    src.MetallicFactor = 0.6f;
    src.RoughnessFactor = 0.7f;
    src.OcclusionStrength = 0.8f;
    src.EmissiveFactor = {0.5f, 0.4f, 0.3f};
    src.NormalScale = 0.9f;

    MaterialTextures textures;
    textures.baseColor = 11u;
    textures.normal = 12u;
    textures.metallicRoughness = 13u;
    textures.occlusion = 14u;
    textures.emissive = 15u;
    textures.hasNormalTexture = true;

    Material material;
    material.Create(nullptr, 3, src, textures);
    const auto &c = material.Constants();

    // BaseWeight stays its own lane — it must NOT be folded into
    // baseColorFactor, or the editor round-trip (Source() -> re-save) and any
    // future per-lobe consumer would read a silently premultiplied colour.
    CHECK(c.baseColorFactor == glm::vec4(0.1f, 0.2f, 0.3f, 0.4f));
    CHECK(c.metalRoughOcclusion.x == 0.6f);
    CHECK(c.metalRoughOcclusion.y == 0.7f);
    CHECK(c.metalRoughOcclusion.z == 0.8f);
    CHECK(c.emissiveFactorNormalScale == glm::vec4(0.5f, 0.4f, 0.3f, 0.9f));
    CHECK(c.texIndices == glm::uvec4(11u, 12u, 13u, 14u));
    CHECK(c.texIndicesEmissive.x == 15u);
    CHECK((c.flags.x & Assisi::Render::kMaterialFlagHasNormalTexture) != 0u);
    CHECK((c.flags.x & Assisi::Render::kMaterialFlagEnergyPreservingDiffuse) == 0u);
}
