/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Render/Material.hpp>

namespace Assisi::Render
{

void Material::Create(nvrhi::IDevice * /*device*/, uint32_t id, const Geometry::MaterialData &source,
                      const MaterialTextures &textures)
{
    _id = id;
    _source = source;
    _textures = textures;

    // Pack the PBR factors + resolved bindless slots into the table row. No GPU
    // upload here — AssetCache writes this value into row `id` of the shared
    // material table (stage D).
    _constants.baseColorFactor = source.BaseColorFactor;
    _constants.emissiveFactorNormalScale = glm::vec4(source.EmissiveFactor, source.NormalScale);
    _constants.metalRoughOcclusion =
        glm::vec4(source.MetallicFactor, source.RoughnessFactor, source.OcclusionStrength, 0.f);
    _constants.flags = glm::uvec4(textures.hasNormalTexture ? 1u : 0u, 0u, 0u, 0u);
    _constants.texIndices =
        glm::uvec4(textures.baseColor, textures.normal, textures.metallicRoughness, textures.occlusion);
    _constants.texIndicesEmissive = glm::uvec4(textures.emissive, 0u, 0u, 0u);

    _created = true;
}

} /* namespace Assisi::Render */
