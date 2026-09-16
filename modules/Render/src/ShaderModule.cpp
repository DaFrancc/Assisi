/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShaderModule.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/AssetSource.hpp>

namespace Assisi::Render
{

nvrhi::ShaderHandle LoadSpirvShader(nvrhi::IDevice *device, const std::string &path, nvrhi::ShaderType stage)
{
    const AssetSource *source = GetAssetSource();
    const std::expected<std::vector<std::byte>, AssetLoadError> spirv =
        source != nullptr ? source->LoadShader(path) : std::unexpected(AssetLoadError::UnknownAsset);
    if (!spirv)
    {
        Core::Log::Error("ShaderModule: failed to load shader '{}' ({}).", path, ToString(spirv.error()));
        return nullptr;
    }

    nvrhi::ShaderDesc desc;
    desc.shaderType = stage;
    desc.debugName = path;
    return device->createShader(desc, spirv->data(), spirv->size());
}

} // namespace Assisi::Render
