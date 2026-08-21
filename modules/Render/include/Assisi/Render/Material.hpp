/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Material.hpp
/// @brief GPU-side material: resolved PBR texture slots + a constants row.
///
/// Built by AssetCache::ResolveMaterial from a Geometry::MaterialData. Channels
/// are bindless-table slots (empty ones resolve to the cache's `prim://` default
/// textures — white / flat normal — so the shader always samples a valid index
/// and never branches on null). The material owns no GPU buffer: it holds its
/// `MaterialConstants` as a plain value and AssetCache writes it into one row of
/// the shared material table (stage D), indexed by the material's id.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/DrawItem.hpp>

namespace Assisi::Render
{

/// @brief Named bits of MaterialConstants::flags.x. Mirrored by the flag
/// constants in mesh.frag — the two must change together.
enum MaterialFlagBits : uint32_t
{
    kMaterialFlagHasNormalTexture = 1u << 0,
    kMaterialFlagEnergyPreservingDiffuse = 1u << 1, ///< EON diffuse; set when BaseDiffuseRoughness > 0.
    kMaterialFlagSpecularAntiAliasing = 1u << 2,    ///< Geometric specular AA; set when the enable is on and the clamp > 0.
};

/// @brief Per-material constants — one row of the shared material table.
/// Mirrors the `MaterialRow` struct in mesh.frag; the two must change
/// together. The layout contract that keeps them trivially in sync: every
/// member is a vec4/uvec4 lane, so the C++ layout, std140 and std430 all
/// agree with no padding, and the row size is wherever sizeof lands — every
/// buffer stride derives from it, nothing hardcodes a byte count. To extend
/// the material model, claim a reserved lane component (or append a new
/// vec4 lane) here and in the shader, and assign it in Material::Create.
/// The texIndices words are each channel's slot in the bindless descriptor
/// table (stage D).
struct MaterialConstants
{
    glm::vec4 baseColorFactor{1.f, 1.f, 1.f, 1.f};
    glm::vec4 emissiveFactorNormalScale{0.f, 0.f, 0.f, 1.f};  ///< xyz = emissive, w = normalScale.
    glm::vec4 metalRoughOcclusion{1.f, 1.f, 1.f, 0.2f};       ///< x = metallic, y = roughness, z = occlusion, w = specular AA variance clamp.
    glm::vec4 specularColorIor{1.f, 1.f, 1.f, 1.5f};          ///< rgb = specularColor, w = specularIor.
    /// x = baseWeight, y = specularWeight, z = baseDiffuseRoughness,
    /// w = alphaCutoff — zero on anything but a masked material, which is the
    /// value that discards nothing.
    glm::vec4 openPbrParams{1.f, 1.f, 0.f, 0.f};
    glm::uvec4 flags{0u, 0u, 0u, 0u};              ///< x = MaterialFlagBits; yzw reserved.
    glm::uvec4 texIndices{0u, 0u, 0u, 0u};         ///< bindless slots: x=baseColor y=normal z=metalRough w=occlusion.
    glm::uvec4 texIndicesEmissive{0u, 0u, 0u, 0u}; ///< x = emissive bindless slot; yzw reserved.
};
static_assert(sizeof(MaterialConstants) % sizeof(glm::vec4) == 0,
              "MaterialConstants must stay vec4-granular so the GLSL mirror shares its layout.");

/// @brief The five texture channels of a PBR material, resolved to their slots
/// in the bindless descriptor table (empty channels resolve to a default
/// texture's slot, so every channel is a valid index).
struct MaterialTextures
{
    uint32_t baseColor = 0;
    uint32_t normal = 0;
    uint32_t metallicRoughness = 0;
    uint32_t occlusion = 0;
    uint32_t emissive = 0;
    bool hasNormalTexture = false;     ///< False when the normal channel is the flat default.
};

class Material
{
public:
    Material() = default;

    /// @brief Compute the constants row and store the resolved textures.
    /// @param id  Dense material-table slot assigned by AssetCache (0-based, reset
    ///            on Clear). Doubles as the opaque sort key's material field, and
    ///            as the index every per-instance record uses to fetch this
    ///            material's row from the GPU material table.
    void Create(nvrhi::IDevice *device, uint32_t id, const Geometry::MaterialData &source,
                const MaterialTextures &textures);

    uint32_t Id() const { return _id; }

    /// @brief The material's per-channel bindless texture slots (indices into the
    /// AssetCache descriptor table), as also packed into the constants row.
    const MaterialTextures &Textures() const { return _textures; }

    /// @brief The material's PBR constants — the exact bytes AssetCache writes
    /// into row `Id()` of the material table. Valid once Create() has run.
    const MaterialConstants &Constants() const { return _constants; }

    /// @brief The CPU material data this was built from (for the editor / re-save).
    const Geometry::MaterialData &Source() const { return _source; }

    /// @brief Whether this material's fragments are alpha-tested.
    bool IsAlphaMasked() const { return _source.Alpha == Geometry::AlphaMode::Mask; }

    /// @brief Which mesh-pass pipeline this material's draws belong in. The one
    /// place the mapping lives, so the CPU draw list and the GPU cull tables
    /// cannot disagree about where a material draws.
    MeshPipeline Pipeline() const { return IsAlphaMasked() ? MeshPipeline::Mask : MeshPipeline::Opaque; }

    bool IsValid() const { return _created; }

private:
    uint32_t _id = 0;
    Geometry::MaterialData _source;
    MaterialTextures _textures;
    MaterialConstants _constants;      ///< The table row; written to the GPU by AssetCache.
    bool _created = false;
};

} /* namespace Assisi::Render */
