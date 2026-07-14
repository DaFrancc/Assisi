/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Material.hpp
/// @brief GPU-side material: resolved PBR textures + a constants buffer.
///
/// Built by AssetCache::ResolveMaterial from a Geometry::MaterialData. Texture
/// pointers are non-owning (AssetCache owns the Textures); empty channels are
/// filled with DefaultResources fallbacks so the shader always samples a valid
/// texture and never branches on null. The constants buffer holds the PBR
/// factors and is laid out to become, verbatim, one row of the future bindless
/// material table (a textureIndices field fills the reserved word then).

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// @brief Per-material constants (std140-friendly, 64 bytes). Mirrors the
/// `MaterialConstants` uniform block in the mesh shader.
struct MaterialConstants
{
    glm::vec4  baseColorFactor{1.f, 1.f, 1.f, 1.f};
    glm::vec4  emissiveFactorNormalScale{0.f, 0.f, 0.f, 1.f}; ///< xyz = emissive, w = normalScale.
    glm::vec4  metalRoughOcclusion{1.f, 1.f, 1.f, 0.f};       ///< x = metallic, y = roughness, z = occlusion, w = pad.
    glm::uvec4 flags{0u, 0u, 0u, 0u};                          ///< bit0 = has normal texture; rest reserved.
};
static_assert(sizeof(MaterialConstants) == 64, "MaterialConstants must stay 64 bytes (bindless table row).");

/// @brief The five texture channels of a PBR material, already resolved to
/// GPU textures (defaults substituted for empty channels).
struct MaterialTextures
{
    nvrhi::ITexture *baseColor = nullptr;
    nvrhi::ITexture *normal = nullptr;
    nvrhi::ITexture *metallicRoughness = nullptr;
    nvrhi::ITexture *occlusion = nullptr;
    nvrhi::ITexture *emissive = nullptr;
    bool             hasNormalTexture = false; ///< False when the normal channel is the flat default.
};

class Material
{
  public:
    Material() = default;

    /// @brief Build the constants buffer and store the resolved textures.
    /// @param id  Stable, process-unique id assigned by AssetCache (never reused;
    ///            survives Clear()). Used to key binding sets.
    void Create(nvrhi::IDevice *device, uint32_t id, const Geometry::MaterialData &source,
                const MaterialTextures &textures);

    uint32_t Id() const { return _id; }

    nvrhi::ITexture *BaseColor() const { return _textures.baseColor; }
    nvrhi::ITexture *Normal() const { return _textures.normal; }
    nvrhi::ITexture *MetallicRoughness() const { return _textures.metallicRoughness; }
    nvrhi::ITexture *Occlusion() const { return _textures.occlusion; }
    nvrhi::ITexture *Emissive() const { return _textures.emissive; }

    nvrhi::IBuffer *Constants() const { return _constants; }

    /// @brief The CPU material data this was built from (for the editor / re-save).
    const Geometry::MaterialData &Source() const { return _source; }

    bool IsValid() const { return _constants != nullptr; }

  private:
    uint32_t               _id = 0;
    Geometry::MaterialData _source;
    MaterialTextures       _textures;
    nvrhi::BufferHandle    _constants;
};

} /* namespace Assisi::Render */
