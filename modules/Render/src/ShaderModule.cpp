/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShaderModule.hpp>

#include <Assisi/Core/Logger.hpp>

#include <fstream>
#include <vector>

namespace Assisi::Render
{

namespace
{
std::vector<char> ReadSpirvFile(const std::string &path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        Assisi::Core::Log::Error("ShaderModule: failed to open shader file: {}", path);
        return {};
    }

    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}
} // namespace

nvrhi::ShaderHandle LoadSpirvShader(nvrhi::IDevice *device, const std::string &path, nvrhi::ShaderType stage)
{
    const std::vector<char> spirv = ReadSpirvFile(path);
    if (spirv.empty())
    {
        return nullptr;
    }

    nvrhi::ShaderDesc desc;
    desc.shaderType = stage;
    desc.debugName = path;
    return device->createShader(desc, spirv.data(), spirv.size());
}

} // namespace Assisi::Render
