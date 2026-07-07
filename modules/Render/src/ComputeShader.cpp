/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ComputeShader.hpp>

#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{

bool ComputeShader::Initialize(nvrhi::IDevice *device, const std::string &spvPath,
                               const nvrhi::BindingLayoutDesc &bindingLayoutDesc)
{
    const nvrhi::ShaderHandle shader = LoadSpirvShader(device, spvPath, nvrhi::ShaderType::Compute);
    if (!shader)
    {
        return false;
    }

    _bindingLayout = device->createBindingLayout(bindingLayoutDesc);

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = shader;
    pipelineDesc.addBindingLayout(_bindingLayout);
    _pipeline = device->createComputePipeline(pipelineDesc);

    return _pipeline != nullptr;
}

void ComputeShader::Dispatch(nvrhi::ICommandList *commandList, nvrhi::IBindingSet *bindingSet, uint32_t groupsX,
                             uint32_t groupsY, uint32_t groupsZ, const void *pushConstants,
                             size_t pushConstantSize) const
{
    nvrhi::ComputeState state;
    state.pipeline = _pipeline;
    state.addBindingSet(bindingSet);
    commandList->setComputeState(state);

    if (pushConstants != nullptr && pushConstantSize > 0)
    {
        commandList->setPushConstants(pushConstants, pushConstantSize);
    }

    commandList->dispatch(groupsX, groupsY, groupsZ);
}

} // namespace Assisi::Render
