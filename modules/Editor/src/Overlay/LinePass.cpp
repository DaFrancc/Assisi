/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/Overlay/LinePass.hpp>

#include <cstddef>
#include <cstdint>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/SceneDepth.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Editor
{

using Render::LoadSpirvShader;

namespace
{
// Vertex-stage push constants (mirrors line.vert). 64 bytes, well within the
// 128-byte push-constant floor.
struct LinePushConstants
{
    glm::mat4 viewProjection;
};

// The thick lines' vertex-stage push constants (mirrors thick_line.vert). 80
// bytes, still within the 128-byte floor.
struct ThickLinePushConstants
{
    glm::mat4 viewProjection;
    glm::vec4 viewport; ///< xy = the framebuffer's size in pixels; zw unused.
};

// How far a depth-tested line is pulled toward the camera, in the depth
// format's smallest steps at the line's own depth. Positive is nearer: the
// scene's depth is reversed (Render/SceneDepth.hpp).
constexpr int32_t kTowardCameraBias = 256;
} // namespace

bool LinePass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &sceneFramebufferInfo,
                          const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath,
                          const std::string &thickVertexShaderSpvPath)
{
    _device = device;

    _vertexShader = LoadSpirvShader(device, vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = LoadSpirvShader(device, pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    _thickVertexShader = LoadSpirvShader(device, thickVertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    if (!_vertexShader || !_pixelShader || !_thickVertexShader)
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

    // In thick_line.vert's location order, which is the order here.
    const nvrhi::VertexAttributeDesc thickAttributes[] = {
        nvrhi::VertexAttributeDesc()
        .setName("COLOR")
        .setFormat(nvrhi::Format::RGBA32_FLOAT)
        .setOffset(offsetof(ThickLineVertex, color))
        .setElementStride(sizeof(ThickLineVertex)),
        nvrhi::VertexAttributeDesc()
        .setName("POSITION")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(ThickLineVertex, position))
        .setElementStride(sizeof(ThickLineVertex)),
        nvrhi::VertexAttributeDesc()
        .setName("OTHER")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(ThickLineVertex, other))
        .setElementStride(sizeof(ThickLineVertex)),
        nvrhi::VertexAttributeDesc()
        .setName("SIDE")
        .setFormat(nvrhi::Format::R32_FLOAT)
        .setOffset(offsetof(ThickLineVertex, side))
        .setElementStride(sizeof(ThickLineVertex)),
        nvrhi::VertexAttributeDesc()
        .setName("HALFWIDTH")
        .setFormat(nvrhi::Format::R32_FLOAT)
        .setOffset(offsetof(ThickLineVertex, halfWidth))
        .setElementStride(sizeof(ThickLineVertex)),
    };
    _thickInputLayout = device->createInputLayout(thickAttributes, static_cast<uint32_t>(std::size(thickAttributes)),
                                                  _thickVertexShader);

    nvrhi::BindingLayoutDesc thickLayoutDesc;
    thickLayoutDesc.visibility = nvrhi::ShaderType::All;
    thickLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ThickLinePushConstants)));
    _thickBindingLayout = device->createBindingLayout(thickLayoutDesc);

    nvrhi::BindingSetDesc thickSetDesc;
    thickSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ThickLinePushConstants)));
    _thickBindingSet = device->createBindingSet(thickSetDesc, _thickBindingLayout);

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
    pipelineDesc.renderState.rasterState.depthBias            = kTowardCameraBias;
    pipelineDesc.renderState.rasterState.slopeScaledDepthBias = 0.0f;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthFunc       = Render::kSceneDepthNearerOrSame;
    _depthTestedPipeline = _device->createGraphicsPipeline(pipelineDesc, sceneFramebufferInfo);

    // On-top variant: depth test off, so the selected collider stays visible
    // through walls (x-ray). No depth bias needed (nothing to fight).
    pipelineDesc.renderState.rasterState.depthBias            = 0;
    pipelineDesc.renderState.rasterState.slopeScaledDepthBias = 0.0f;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    _onTopPipeline = _device->createGraphicsPipeline(pipelineDesc, sceneFramebufferInfo);

    // The thick lines: the same two depth modes over triangles, through their own
    // vertex stage and layout. The bias stays on the depth-tested one for the
    // same coplanar edges.
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = _thickVertexShader;
    pipelineDesc.inputLayout = _thickInputLayout;
    // In the one slot the thin pipelines' layout held.
    pipelineDesc.bindingLayouts[0] = _thickBindingLayout;
    pipelineDesc.renderState.rasterState.depthBias = kTowardCameraBias;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    _thickDepthTestedPipeline = _device->createGraphicsPipeline(pipelineDesc, sceneFramebufferInfo);
    pipelineDesc.renderState.rasterState.depthBias = 0;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    _thickOnTopPipeline = _device->createGraphicsPipeline(pipelineDesc, sceneFramebufferInfo);

    if (!IsValid())
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

bool LinePass::EnsureThickCapacity(int32_t slot, uint32_t cornerCount)
{
    if (cornerCount <= _thickCapacities[slot] && _thickBuffers[slot] != nullptr)
    {
        return true;
    }

    // Grown with headroom, as the thin buffers are. Six corners per segment.
    uint32_t newCapacity = _thickCapacities[slot] == 0 ? 6 * 256 : _thickCapacities[slot];
    while (newCapacity < cornerCount)
    {
        newCapacity *= 2;
    }

    nvrhi::BufferDesc desc;
    desc.byteSize         = static_cast<size_t>(newCapacity) * sizeof(ThickLineVertex);
    desc.isVertexBuffer   = true;
    desc.initialState     = nvrhi::ResourceStates::VertexBuffer;
    desc.keepInitialState = true;
    desc.debugName        = "LinePass::ThickCorners";
    nvrhi::BufferHandle grown = _device->createBuffer(desc);
    if (grown == nullptr)
    {
        Core::Log::Error("LinePass: failed to grow the thick line buffer to {} corners.", newCapacity);
        return false;
    }
    _thickBuffers[slot]    = grown;
    _thickCapacities[slot] = newCapacity;
    return true;
}

void LinePass::DrawThick(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                         std::span<const ThickLineVertex> corners, bool onTop)
{
    const uint32_t cornerCount = static_cast<uint32_t>(corners.size());
    if (!IsValid() || cornerCount == 0)
    {
        return;
    }
    const int32_t slot = onTop ? 1 : 0;
    if (!EnsureThickCapacity(slot, cornerCount))
    {
        return;
    }

    frame.commandList->writeBuffer(_thickBuffers[slot], corners.data(),
                                   static_cast<size_t>(cornerCount) * sizeof(ThickLineVertex));

    nvrhi::GraphicsState state;
    state.pipeline    = onTop ? _thickOnTopPipeline : _thickDepthTestedPipeline;
    state.framebuffer = frame.framebuffer;
    state.addBindingSet(_thickBindingSet);
    state.addVertexBuffer(nvrhi::VertexBufferBinding{_thickBuffers[slot], 0, 0});
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    frame.commandList->setGraphicsState(state);

    ThickLinePushConstants pc;
    pc.viewProjection = viewProjection;
    pc.viewport = glm::vec4(static_cast<float>(frame.width), static_cast<float>(frame.height), 0.f, 0.f);
    frame.commandList->setPushConstants(&pc, sizeof(pc));

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = cornerCount;
    frame.commandList->draw(drawArgs);
}

void LinePass::Draw(const Render::RenderFrame &frame, const glm::mat4 &viewProjection, std::span<const LineVertex> vertices,
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

} // namespace Assisi::Editor
