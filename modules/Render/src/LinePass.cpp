/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/LinePass.hpp>

#include <cstddef>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{

namespace
{
// Vertex-stage push constants (mirrors line.vert). 64 bytes, well within the
// 128-byte push-constant floor.
struct LinePushConstants
{
    glm::mat4 viewProjection;
};
} // namespace

bool LinePass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &sceneFramebufferInfo,
                          const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath)
{
    _device = device;

    _vertexShader = LoadSpirvShader(device, vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = LoadSpirvShader(device, pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        return false;
    }

    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
        .setName("POSITION")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(LineVertex, position))
        .setElementStride(sizeof(LineVertex)),
        nvrhi::VertexAttributeDesc()
        .setName("COLOR")
        .setFormat(nvrhi::Format::RGBA32_FLOAT)
        .setOffset(offsetof(LineVertex, color))
        .setElementStride(sizeof(LineVertex)),
    };
    _inputLayout = device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), _vertexShader);

    // Push constants only (the view-projection); colour rides in the vertices.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(LinePushConstants)));
    _bindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(LinePushConstants)));
    _bindingSet = device->createBindingSet(setDesc, _bindingLayout);

    return BuildPipelines(sceneFramebufferInfo);
}

bool LinePass::BuildPipelines(const nvrhi::FramebufferInfo &sceneFramebufferInfo)
{
    // Shared state: line topology, no cull, alpha-blended composite into the scene
    // target. The two pipelines differ only in depth-test enable and depth bias —
    // neither writes depth, so lines never occlude the scene or each other.
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::LineList;
    pipelineDesc.VS       = _vertexShader;
    pipelineDesc.PS       = _pixelShader;
    pipelineDesc.inputLayout = _inputLayout;
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode               = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::BlendState::RenderTarget &blend = pipelineDesc.renderState.blendState.targets[0];
    blend.blendEnable    = true;
    blend.srcBlend       = nvrhi::BlendFactor::SrcAlpha;
    blend.destBlend      = nvrhi::BlendFactor::InvSrcAlpha;
    blend.blendOp        = nvrhi::BlendOp::Add;
    blend.srcBlendAlpha  = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
    blend.blendOpAlpha   = nvrhi::BlendOp::Add;

    // Depth-tested variant: occluded by scene geometry, so unselected colliders
    // recede behind the world. A collider edge is often coplanar with the mesh it
    // wraps (a floor box's rim on the floor surface); a small CONSTANT bias toward
    // the camera keeps those lines from z-fighting the surface without a slope-scaled
    // term, which at grazing angles would pull the lines in front of everything.
    pipelineDesc.renderState.rasterState.depthBias            = -256;
    pipelineDesc.renderState.rasterState.slopeScaledDepthBias = 0.0f;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthFunc       = nvrhi::ComparisonFunc::LessOrEqual;
    _depthTestedPipeline = _device->createGraphicsPipeline(pipelineDesc, sceneFramebufferInfo);

    // On-top variant: depth test off, so the selected collider stays visible
    // through walls (x-ray). No depth bias needed (nothing to fight).
    pipelineDesc.renderState.rasterState.depthBias            = 0;
    pipelineDesc.renderState.rasterState.slopeScaledDepthBias = 0.0f;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    _onTopPipeline = _device->createGraphicsPipeline(pipelineDesc, sceneFramebufferInfo);

    if (_depthTestedPipeline == nullptr || _onTopPipeline == nullptr)
    {
        Core::Log::Error("LinePass: failed to create the line pipelines.");
        return false;
    }
    return true;
}

bool LinePass::RebuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo)
{
    if (_bindingLayout == nullptr)
    {
        return true; // nothing built yet — nothing to rebuild
    }
    return BuildPipelines(sceneFramebufferInfo);
}

bool LinePass::EnsureVertexCapacity(int32_t slot, uint32_t vertexCount)
{
    if (vertexCount <= _vertexCapacities[slot] && _vertexBuffers[slot] != nullptr)
    {
        return true;
    }

    // Grow with headroom so a scene whose collider count creeps up doesn't
    // reallocate every frame.
    uint32_t newCapacity = _vertexCapacities[slot] == 0 ? 256 : _vertexCapacities[slot];
    while (newCapacity < vertexCount)
    {
        newCapacity *= 2;
    }

    nvrhi::BufferDesc desc;
    desc.byteSize         = static_cast<size_t>(newCapacity) * sizeof(LineVertex);
    desc.isVertexBuffer   = true;
    desc.initialState     = nvrhi::ResourceStates::VertexBuffer;
    desc.keepInitialState = true;
    desc.debugName        = "LinePass::Vertices";
    nvrhi::BufferHandle grown = _device->createBuffer(desc);
    if (grown == nullptr)
    {
        Core::Log::Error("LinePass: failed to grow the vertex buffer to {} vertices.", newCapacity);
        return false;
    }
    _vertexBuffers[slot]    = grown;
    _vertexCapacities[slot] = newCapacity;
    return true;
}

void LinePass::Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, std::span<const LineVertex> vertices,
                    bool onTop)
{
    // Drop a trailing unpaired vertex: a LineList needs whole segments.
    const uint32_t vertexCount = static_cast<uint32_t>(vertices.size() & ~static_cast<size_t>(1));
    if (!IsValid() || vertexCount == 0)
    {
        return;
    }
    const int32_t slot = onTop ? 1 : 0;
    if (!EnsureVertexCapacity(slot, vertexCount))
    {
        return;
    }

    frame.commandList->writeBuffer(_vertexBuffers[slot], vertices.data(),
                                   static_cast<size_t>(vertexCount) * sizeof(LineVertex));

    nvrhi::GraphicsState state;
    state.pipeline    = onTop ? _onTopPipeline : _depthTestedPipeline;
    state.framebuffer = frame.framebuffer;
    state.addBindingSet(_bindingSet);
    state.addVertexBuffer(nvrhi::VertexBufferBinding{_vertexBuffers[slot], 0, 0});
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    frame.commandList->setGraphicsState(state);

    LinePushConstants pc;
    pc.viewProjection = viewProjection;
    frame.commandList->setPushConstants(&pc, sizeof(pc));

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = vertexCount;
    frame.commandList->draw(drawArgs);
}

} // namespace Assisi::Render
