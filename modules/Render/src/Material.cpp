/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Render/Material.hpp>

namespace Assisi::Render
{

Geometry::MaterialData ShownMaterialSource(const Geometry::MaterialData &base, SurfaceLook look)
{
    Geometry::MaterialData shown = base;
    if (look == SurfaceLook::Light)
    {
        // The texture's light through an emissive factor of one, over a body
        // that adds nothing of its own.
        shown.BaseColorFactor = {0.f, 0.f, 0.f, 1.f};
        shown.MetallicFactor = 0.f;
        shown.SpecularWeight = 0.f;
        shown.EmissiveFactor = {1.f, 1.f, 1.f};
        shown.Alpha = Geometry::AlphaMode::Opaque;
    }
    return shown;
}

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
    _constants.metalRoughOcclusion = glm::vec4(source.MetallicFactor, source.RoughnessFactor,
                                               source.OcclusionStrength, source.SpecularAaVarianceClamp);
    _constants.specularColorIor = glm::vec4(source.SpecularColor, source.SpecularIor);
    // The cutoff rides in openPbrParams.w, and is zero unless the material is
    // masked: the masked shader tests `alpha < cutoff`, so zero kills nothing and
    // an opaque material stays solid even if that pipeline ever draws it.
    const float alphaCutoff =
        source.Alpha == Geometry::AlphaMode::Mask ? source.AlphaCutoff : 0.f;
    _constants.openPbrParams =
        glm::vec4(source.BaseWeight, source.SpecularWeight, source.BaseDiffuseRoughness, alphaCutoff);

    uint32_t flags = 0u;
    if (textures.hasNormalTexture)
        flags |= kMaterialFlagHasNormalTexture;
    // The EON lobe costs per light, so it is opt-in per material: a zero
    // diffuse roughness is exactly Lambert and skips it.
    if (source.BaseDiffuseRoughness > 0.f)
        flags |= kMaterialFlagEnergyPreservingDiffuse;
    // Specular AA costs four screen-space derivatives per fragment, so it is
    // gated on the clamp too: a zero clamp admits no widening, and taking the
    // derivatives to add nothing is pure cost.
    if (source.SpecularAntiAliasing && source.SpecularAaVarianceClamp > 0.f)
        flags |= kMaterialFlagSpecularAntiAliasing;
    if (source.ShadowsFollowSmoothing)
    {
        flags |= kMaterialFlagShadowsFollowSmoothing;
    }
    if (source.EmissiveFactor != glm::vec3(0.f))
    {
        flags |= kMaterialFlagHasEmissive;
    }
    if (textures.hasOcclusionTexture && source.OcclusionStrength != 0.f)
    {
        flags |= kMaterialFlagHasOcclusion;
    }
    _constants.flags = glm::uvec4(flags, 0u, 0u, 0u);
    _constants.texIndices =
        glm::uvec4(textures.baseColor, textures.normal, textures.metallicRoughness, textures.occlusion);
    _constants.texIndicesEmissive = glm::uvec4(textures.emissive, 0u, 0u, 0u);

    _created = true;
}

} /* namespace Assisi::Render */
