/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MaterialData.hpp
/// @brief CPU-side material description — factors + texture paths, no GPU types.
///
/// This is the decode target the mesh importer fills from glTF
/// pbrMetallicRoughness, the content of a .amat asset file, and the input the
/// renderer turns into a GPU Material. It lives in Geometry so importers,
/// tools, and tests can produce/consume materials without linking the
/// renderer. See docs/mesh-material-architecture.md §2.

#include <string>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief One material: PBR factors plus a GUID reference for each texture
///        channel. A nil id means "channel is factor-only" — the renderer
///        substitutes a default texture (white / flat normal), so shaders never
///        branch on missing channels.
///
/// Field initialisers are the glTF pbrMetallicRoughness *spec* defaults
/// (metallic = 1, roughness = 1) — correct when a glTF omits fields. The
/// engine's fallback material for empty/missing references is a separate,
/// deliberately different set (white, metallic 0, roughness 0.6) owned by the
/// renderer's AssetCache (see AssetCache::FallbackMaterial).
///
/// Colour space is a fixed property of each channel, never per-file
/// configuration: baseColor and emissive are sRGB; normal, metallic-roughness,
/// and occlusion are linear.
///
/// OpenPBR: this is the base + specular + emission subset of the OpenPBR
/// Surface base layer, and deliberately nothing more. Coat, fuzz, anisotropy,
/// thin-film, transmission, subsurface, and dispersion are out of scope by
/// decision, not oversight — the schema is default-on-missing, so each can
/// land later as a one-field-plus-one-lobe increment with zero content
/// migration. Coat/fuzz are the only candidates worth revisiting, and only
/// once reflection probes (lighting L5) give them an environment to reflect.
/// The pre-OpenPBR field names stay (BaseColorFactor ≡ base_color,
/// MetallicFactor ≡ base_metalness, RoughnessFactor ≡ specular_roughness):
/// renaming is silent data loss under the schema's ignore-unknown-keys rule,
/// for zero benefit.
AASSET()
struct MaterialData
{
    // --- Factors (glTF pbrMetallicRoughness + friends) ---
    AFIELD() glm::vec4 BaseColorFactor{1.f, 1.f, 1.f, 1.f};
    AFIELD() float MetallicFactor = 1.f;
    AFIELD() float RoughnessFactor = 1.f;
    AFIELD() float NormalScale = 1.f;
    AFIELD() float OcclusionStrength = 1.f;
    AFIELD() glm::vec3 EmissiveFactor{0.f, 0.f, 0.f};

    // --- Factors (OpenPBR base layer) ---
    // Defaults reproduce the pre-OpenPBR shading exactly: weights 1,
    // specular_ior 1.5 -> F0 0.04, diffuse roughness 0 = Lambert. A material
    // that never sets them renders unchanged.
    AFIELD() float BaseWeight = 1.f;                  ///< OpenPBR base_weight.
    AFIELD() float SpecularWeight = 1.f;              ///< OpenPBR specular_weight.
    AFIELD() glm::vec3 SpecularColor{1.f, 1.f, 1.f};  ///< OpenPBR specular_color; the F82 edge tint on metals.
    AFIELD() float SpecularIor = 1.5f;                ///< OpenPBR specular_ior.
    AFIELD() float BaseDiffuseRoughness = 0.f;        ///< OpenPBR base_diffuse_roughness; > 0 enables EON diffuse.

    // --- Texture channels (GUID references; nil = factor-only) ---
    AFIELD() Assisi::Core::AssetId BaseColorTexture;         ///< sRGB.
    AFIELD() Assisi::Core::AssetId NormalTexture;            ///< Linear; xyz in [0,1] -> *2-1.
    AFIELD() Assisi::Core::AssetId MetallicRoughnessTexture; ///< Linear; glTF packing: G = roughness, B = metallic.
    AFIELD() Assisi::Core::AssetId OcclusionTexture;         ///< Linear; R channel (often the same file as MR).
    AFIELD() Assisi::Core::AssetId EmissiveTexture;          ///< sRGB.

    /// @brief Display label (the glTF material name). UI only — never
    /// serialized into .amat, never a lookup key, so deliberately not AFIELD.
    std::string Name;
};

} /* namespace Assisi::Geometry */
