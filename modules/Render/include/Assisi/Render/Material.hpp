/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Material.hpp
/// @brief GPU-side material: resolved PBR texture slots + a constants row.
///
/// Built by AssetCache::ResolveMaterial from a Geometry::MaterialData. Channels
/// are bindless-table slots (empty ones resolve to the cache's `prim://` default
/// textures — white / flat normal — so the shader always samples a valid index
/// and never branches on null). The material no longer owns a GPU buffer: it
/// holds its `MaterialConstants` as a plain value and AssetCache writes it into
/// one row of the shared material table (stage D), indexed by the material's id.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// @brief Per-material constants (std140-friendly, 96 bytes). Mirrors the
/// `MaterialConstants` uniform block in the mesh shader. The texIndices words
/// are each channel's slot in the bindless descriptor table (stage D).
struct MaterialConstants
{
    glm::vec4 baseColorFactor{1.f, 1.f, 1.f, 1.f};
    glm::vec4 emissiveFactorNormalScale{0.f, 0.f, 0.f, 1.f};  ///< xyz = emissive, w = normalScale.
    glm::vec4 metalRoughOcclusion{1.f, 1.f, 1.f, 0.f};        ///< x = metallic, y = roughness, z = occlusion, w = pad.
    glm::uvec4 flags{0u, 0u, 0u, 0u};                          ///< bit0 = has normal texture; rest reserved.
    glm::uvec4 texIndices{0u, 0u, 0u, 0u};        ///< bindless slots: x=baseColor y=normal z=metalRough w=occlusion.
    glm::uvec4 texIndicesEmissive{0u, 0u, 0u, 0u}; ///< x = emissive bindless slot; yzw reserved.
};
static_assert(sizeof(MaterialConstants) == 96, "MaterialConstants must match the shader's uniform block.");

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

    bool IsValid() const { return _created; }

private:
    uint32_t _id = 0;
    Geometry::MaterialData _source;
    MaterialTextures _textures;
    MaterialConstants _constants;      ///< The table row; written to the GPU by AssetCache.
    bool _created = false;
};

} /* namespace Assisi::Render */
