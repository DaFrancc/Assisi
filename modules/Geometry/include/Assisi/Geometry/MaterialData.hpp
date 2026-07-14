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

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief One material: PBR factors plus virtual asset paths for each texture
///        channel. An empty path means "channel is factor-only" — the renderer
///        substitutes a default texture (white / flat normal), so shaders never
///        branch on missing channels.
///
/// Field initialisers are the glTF pbrMetallicRoughness *spec* defaults
/// (metallic = 1, roughness = 1) — correct when a glTF omits fields. The
/// engine's fallback material for empty/missing references is a separate,
/// deliberately different set (white, metallic 0, roughness 0.6) owned by the
/// renderer's DefaultResources.
///
/// Colour space is a fixed property of each channel, never per-file
/// configuration: baseColor and emissive are sRGB; normal, metallic-roughness,
/// and occlusion are linear.
AASSET()
struct MaterialData
{
    // --- Factors (glTF pbrMetallicRoughness + friends) ---
    AFIELD() glm::vec4 BaseColorFactor{1.f, 1.f, 1.f, 1.f};
    AFIELD() float     MetallicFactor = 1.f;
    AFIELD() float     RoughnessFactor = 1.f;
    AFIELD() float     NormalScale = 1.f;
    AFIELD() float     OcclusionStrength = 1.f;
    AFIELD() glm::vec3 EmissiveFactor{0.f, 0.f, 0.f};

    // --- Texture channels (virtual asset paths; empty = factor-only) ---
    AFIELD() Assisi::Core::AssetPath BaseColorTexture;         ///< sRGB.
    AFIELD() Assisi::Core::AssetPath NormalTexture;            ///< Linear; xyz in [0,1] -> *2-1.
    AFIELD() Assisi::Core::AssetPath MetallicRoughnessTexture; ///< Linear; glTF packing: G = roughness, B = metallic.
    AFIELD() Assisi::Core::AssetPath OcclusionTexture;         ///< Linear; R channel (often the same file as MR).
    AFIELD() Assisi::Core::AssetPath EmissiveTexture;          ///< sRGB.

    /// @brief Display label (the glTF material name). UI only — never
    /// serialized into .amat, never a lookup key, so deliberately not AFIELD.
    std::string Name;
};

} /* namespace Assisi::Geometry */
