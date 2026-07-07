/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShaderModule.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <vector>

namespace Assisi::Render
{

namespace
{
EShLanguage ToGlslangStage(nvrhi::ShaderType stage)
{
    switch (stage)
    {
    case nvrhi::ShaderType::Vertex:
        return EShLangVertex;
    case nvrhi::ShaderType::Pixel:
        return EShLangFragment;
    case nvrhi::ShaderType::Compute:
        return EShLangCompute;
    default:
        Assisi::Core::Log::Error("CompileGlslShader: unsupported nvrhi::ShaderType.");
        return EShLangVertex;
    }
}
} // namespace

nvrhi::ShaderHandle CompileGlslShader(nvrhi::IDevice *device, std::string_view glslVirtualPath,
                                      nvrhi::ShaderType stage)
{
    const auto src = Assisi::Core::AssetSystem::ReadText(glslVirtualPath);
    if (!src)
    {
        Assisi::Core::Log::Error("CompileGlslShader: failed to read '{}'.", glslVirtualPath);
        return nullptr;
    }

    const EShLanguage lang = ToGlslangStage(stage);
    glslang::TShader shader(lang);

    const char *srcPtr = src->c_str();
    shader.setStrings(&srcPtr, 1);
    // Matches the Vulkan/SPIR-V baseline the old `glslang-standalone -V` build-time
    // invocation targeted by default (no `--target-env` flag was ever passed) —
    // switching to runtime compilation shouldn't also silently change what SPIR-V
    // version/capabilities get emitted.
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    constexpr auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 100, false, messages))
    {
        Assisi::Core::Log::Error("CompileGlslShader: failed to compile '{}':\n{}", glslVirtualPath,
                                 shader.getInfoLog());
        return nullptr;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
    {
        Assisi::Core::Log::Error("CompileGlslShader: failed to link '{}':\n{}", glslVirtualPath,
                                 program.getInfoLog());
        return nullptr;
    }

    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);

    nvrhi::ShaderDesc desc;
    desc.shaderType = stage;
    desc.debugName = std::string(glslVirtualPath);
    return device->createShader(desc, spirv.data(), spirv.size() * sizeof(uint32_t));
}

} // namespace Assisi::Render
