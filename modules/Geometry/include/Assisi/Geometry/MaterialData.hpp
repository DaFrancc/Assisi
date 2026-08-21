/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MaterialData.hpp
/// @brief CPU-side material description — factors + texture paths, no GPU types.
///
/// This is the decode target the mesh importer fills from glTF
/// pbrMetallicRoughness, the content of a .amat asset file, and the input the
/// renderer turns into a GPU Material. It lives in Geometry so importers,
/// tools, and tests can produce/consume materials without linking the
/// renderer.

#include <cstdint>
#include <string>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

/// @brief How a surface's alpha is resolved.
///
/// Opaque ignores alpha entirely. Mask is a per-fragment kill test against
/// MaterialData::AlphaCutoff — the cutout of foliage, chain-link and decals —
/// and draws through its own pipeline, because a shader that can discard costs
/// the whole pipeline its early depth rejection.
///
/// glTF's third mode, BLEND, is deliberately absent: there is no blended pass to
/// draw it in, and an enumerator that silently rendered opaque would be a field
/// that lies. It arrives with the pass. Enumerators serialize as their integer
/// value, so appending one leaves every existing .amat reading the same.
AENUM()
enum class AlphaMode : std::uint8_t
{
    Opaque, ///< Alpha is ignored; the surface is solid.
    Mask,   ///< Alpha below AlphaCutoff kills the fragment.
};

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
/// Surface base layer. The remaining lobes — coat, fuzz, anisotropy,
/// thin-film, subsurface, transmission, and dispersion — are planned rather
/// than excluded. The schema is default-on-missing, which is what keeps that
/// cheap: each arrives as one field plus one lobe, and a material authored
/// before a lobe exists deserializes identically once it lands, so no
/// content migrates.
/// The pre-OpenPBR field names stay (BaseColorFactor ≡ base_color,
/// MetallicFactor ≡ base_metalness, RoughnessFactor ≡ specular_roughness):
/// renaming is silent data loss under the schema's ignore-unknown-keys rule,
/// for zero benefit.
AASSET()
struct MaterialData
{
    // --- Factors (glTF pbrMetallicRoughness + friends) ---
    // The min/max are editor clamps, not load-time validation: an importer may
    // still deliver an out-of-range value, and the renderer's math is what has
    // to survive that. They exist so an author cannot *create* one by dragging.
    AFIELD() Assisi::Math::Color4 BaseColorFactor{1.f, 1.f, 1.f, 1.f};
    AFIELD(min = 0, max = 1) float MetallicFactor = 1.f;
    AFIELD(min = 0, max = 1) float RoughnessFactor = 1.f;
    AFIELD(min = 0) float NormalScale = 1.f;
    AFIELD(min = 0, max = 1) float OcclusionStrength = 1.f;
    AFIELD() Assisi::Math::Color3 EmissiveFactor{0.f, 0.f, 0.f};

    // --- Factors (OpenPBR base layer) ---
    // Defaults reproduce the pre-OpenPBR shading exactly: weights 1,
    // specular_ior 1.5 -> F0 0.04, diffuse roughness 0 = Lambert. A material
    // that never sets them renders unchanged.
    AFIELD(min = 0, max = 1) float BaseWeight = 1.f;                  ///< OpenPBR base_weight.
    AFIELD(min = 0, max = 1) float SpecularWeight = 1.f;              ///< OpenPBR specular_weight.
    AFIELD() Assisi::Math::Color3 SpecularColor{1.f, 1.f, 1.f};       ///< OpenPBR specular_color; the F82 edge tint on metals.
    /// OpenPBR specular_ior. Bounded to the spec's [1, 3]: below 1 inverts the
    /// Fresnel the F0 derivation assumes, and no dielectric this model covers
    /// reaches 3.
    AFIELD(min = 1, max = 3) float SpecularIor = 1.5f;
    AFIELD(min = 0, max = 1) float BaseDiffuseRoughness = 0.f;        ///< OpenPBR base_diffuse_roughness; > 0 enables EON diffuse.

    // --- Geometric specular antialiasing ---
    // Not an OpenPBR parameter: a filtering decision about how the specular lobe
    // is sampled, not a property of the surface. On by default because a glossy
    // normal-mapped surface sparkles without it, and the engine has no temporal
    // accumulation to hide that.
    AFIELD() bool SpecularAntiAliasing = true;
    /// Ceiling on the roughness variance the filter may add (GGX alpha-squared
    /// units). It bounds how far a high-curvature surface can be blurred toward
    /// matte: without it, a normal map at a grazing angle drives the lobe to
    /// fully rough and the material stops looking like metal. Zero disables the
    /// filter as surely as clearing SpecularAntiAliasing does.
    AFIELD(min = 0, max = 1) float SpecularAaVarianceClamp = 0.2f;

    // --- Alpha ---
    // OpenPBR's geometry_opacity, cutout half. Opaque is the default, so every
    // material authored before these existed keeps drawing solid.
    AFIELD() AlphaMode Alpha = AlphaMode::Opaque;
    /// Fragments whose base-colour alpha falls below this are killed. Read only
    /// when Alpha is Mask; the glTF default, so an import that omits it agrees
    /// with the exporter rather than with us.
    AFIELD(min = 0, max = 1) float AlphaCutoff = 0.5f;

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
