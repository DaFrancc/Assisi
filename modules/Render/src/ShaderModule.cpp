/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShaderModule.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstdint>

namespace Assisi::Render
{

nvrhi::ShaderHandle LoadSpirvShader(nvrhi::IDevice *device, const std::string &path, nvrhi::ShaderType stage)
{
    // Compiled .spv lives under the asset root (assets/shaders/), so it resolves
    // through AssetSystem like every other asset — no CWD dependency.
    const std::expected<std::vector<std::byte>, Core::AssetError> spirv = Core::AssetSystem::ReadBinary(path);
    if (!spirv)
    {
        Core::Log::Error("ShaderModule: failed to load shader '{}' (asset error {}).", path,
                         static_cast<int32_t>(spirv.error()));
        return nullptr;
    }

    nvrhi::ShaderDesc desc;
    desc.shaderType = stage;
    desc.debugName = path;
    return device->createShader(desc, spirv->data(), spirv->size());
}

} // namespace Assisi::Render
