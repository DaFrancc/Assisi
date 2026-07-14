/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Render/Material.hpp>

namespace Assisi::Render
{

void Material::Create(nvrhi::IDevice *device, uint32_t id, const Geometry::MaterialData &source,
                      const MaterialTextures &textures)
{
    _id = id;
    _source = source;
    _textures = textures;

    MaterialConstants constants;
    constants.baseColorFactor = source.BaseColorFactor;
    constants.emissiveFactorNormalScale =
        glm::vec4(source.EmissiveFactor, source.NormalScale);
    constants.metalRoughOcclusion =
        glm::vec4(source.MetallicFactor, source.RoughnessFactor, source.OcclusionStrength, 0.f);
    constants.flags = glm::uvec4(textures.hasNormalTexture ? 1u : 0u, 0u, 0u, 0u);

    nvrhi::BufferDesc desc;
    desc.byteSize = sizeof(MaterialConstants);
    desc.isConstantBuffer = true;
    desc.debugName = "Material::Constants";
    desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    desc.keepInitialState = true;
    _constants = device->createBuffer(desc);

    nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    commandList->writeBuffer(_constants, &constants, sizeof(constants));
    commandList->close();
    device->executeCommandList(commandList);
}

} /* namespace Assisi::Render */
