/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/SkyPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{

bool SkyPass::Initialize(const InitParams &params)
{
    _device = params.device;

    _vertexShader = LoadSpirvShader(_device, params.vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = LoadSpirvShader(_device, params.pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        return false;
    }

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(SkyConstants);
    constantsDesc.isConstantBuffer = true;
    constantsDesc.debugName = "SkyPass::Constants";
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    _constantsBuffer = _device->createBuffer(constantsDesc);
    if (_constantsBuffer == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the constants buffer.");
        return false;
    }

    // Both stages read the block: the vertex shader for the inverse
    // view-projection, the pixel shader for everything else.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    _bindingLayout = _device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _constantsBuffer));
    _bindingSet = _device->createBindingSet(setDesc, _bindingLayout);
    if (_bindingLayout == nullptr || _bindingSet == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky binding set.");
        return false;
    }

    return BuildPipeline(params.framebufferInfo);
}

bool SkyPass::BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = _vertexShader;
    pipelineDesc.PS = _pixelShader;
    pipelineDesc.addBindingLayout(_bindingLayout);

    // No input layout and no vertex buffer: the triangle comes from
    // gl_VertexIndex. Culling is off because that triangle's winding is a
    // property of the index arithmetic rather than of anything a caller controls.
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;

    // Equal against the 1.0 the depth clear left. The scene tests Less and so
    // wrote something smaller wherever it drew, which is what makes covered
    // pixels fail here and cost nothing. Depth is not written, so the scene's
    // depth reaches the passes after this one intact.
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
    pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Equal;

    _pipeline = _device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (_pipeline == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky pipeline.");
        return false;
    }
    return true;
}

bool SkyPass::RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (_bindingLayout == nullptr)
    {
        return true; // nothing built yet — nothing to rebuild
    }
    return BuildPipeline(framebufferInfo);
}

void SkyPass::Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &cameraPosition,
                   const SkySun &sun, const SkySettings &settings)
{
    if (!IsValid())
    {
        return;
    }

    ASSISI_PROFILE_GPU_PASS(frame.commandList, "sky");

    const SkyConstants constants = MakeSkyConstants(glm::inverse(viewProjection), cameraPosition, sun, settings);
    frame.commandList->writeBuffer(_constantsBuffer, &constants, sizeof(constants));

    nvrhi::GraphicsState state;
    state.pipeline = _pipeline;
    state.framebuffer = frame.framebuffer;
    state.addBindingSet(_bindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    frame.commandList->setGraphicsState(state);

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = 3;
    frame.commandList->draw(drawArgs);
}

} // namespace Assisi::Render
