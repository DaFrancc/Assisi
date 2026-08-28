/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowDepthRenderer.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace Assisi::Render
{
namespace
{
/// Starting capacity of the instance and indirect-args buffers, in records.
/// Grown geometrically past this; a first level typically fits without one.
constexpr std::uint32_t kInitialCasterCapacity = 1024u;
} // namespace

void ShadowDrawList::Clear()
{
    instances.clear();
    commands.clear();
    commandVertexBuffers.clear();
    commandIndexBuffers.clear();
    viewCommandStart.clear();
    culled = 0;
}

void BuildShadowDrawList(std::span<const ShadowDepthTarget> targets, std::span<const ShadowCaster> casters,
                         ShadowDrawList &out)
{
    out.Clear();
    out.viewCommandStart.reserve(targets.size() + 1u);

    for (const ShadowDepthTarget &target : targets)
    {
        out.viewCommandStart.push_back(static_cast<std::uint32_t>(out.commands.size()));

        const Frustum frustum = Frustum::FromViewProjection(target.view.viewProjection);

        const ShadowCaster *run = nullptr;
        for (const ShadowCaster &caster : casters)
        {
            if (caster.indexCount == 0)
            {
                continue;
            }
            if (!frustum.IntersectsSphere(caster.worldSphere))
            {
                ++out.culled;
                run = nullptr; // a gap in the run: the next survivor opens a new batch
                continue;
            }

            const auto instanceIndex = static_cast<std::uint32_t>(out.instances.size());
            out.instances.push_back(ShadowInstanceData{.model = caster.model});

            if (run != nullptr && SameShadowGeometry(*run, caster))
            {
                ++out.commands.back().instanceCount;
            }
            else
            {
                nvrhi::DrawIndexedIndirectArguments command;
                command.indexCount = caster.indexCount;
                command.instanceCount = 1;
                command.startIndexLocation = caster.startIndexLocation;
                command.baseVertexLocation = caster.baseVertexLocation;
                command.startInstanceLocation = instanceIndex;
                out.commands.push_back(command);
                out.commandVertexBuffers.push_back(caster.vertexBuffer);
                out.commandIndexBuffers.push_back(caster.indexBuffer);
                run = &caster;
            }
        }
    }

    out.viewCommandStart.push_back(static_cast<std::uint32_t>(out.commands.size()));
}

bool ShadowDepthRenderer::Initialize(const InitParams &params)
{
    _device = params.device;
    if (_device == nullptr)
    {
        return false;
    }

    _vertexShader = LoadSpirvShader(_device, params.vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    if (!_vertexShader)
    {
        return false;
    }

    // Position only. A depth pass has no use for normals, tangents or texture
    // coordinates, and reading fewer attributes is most of why it is cheap.
    using Assisi::Geometry::Vertex;
    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
        .setName("POSITION")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(Vertex, Position))
        .setElementStride(sizeof(Vertex)),
    };
    _inputLayout = _device->createInputLayout(attributes, static_cast<std::uint32_t>(std::size(attributes)),
                                              _vertexShader);

    // t0 = per-instance world matrices, plus the view matrix as a push constant.
    // Nothing else: no material, no lights, no frame constants.
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::mat4)));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    _bindingLayout = _device->createBindingLayout(bindingLayoutDesc);
    if (_bindingLayout == nullptr)
    {
        Core::Log::Error("ShadowDepthRenderer: failed to create the binding layout.");
        return false;
    }

    return true;
}

nvrhi::GraphicsPipelineHandle ShadowDepthRenderer::CreatePipeline(nvrhi::IFramebuffer *prototype,
                                                                  float slopeBias) const
{
    if (!IsReady() || prototype == nullptr)
    {
        return nullptr;
    }

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.inputLayout = _inputLayout;
    pipelineDesc.VS = _vertexShader;
    // No PS: nothing but depth is written, so there is no fragment stage at all.
    pipelineDesc.addBindingLayout(_bindingLayout);
    // Back faces, so what the map records is the surface the light actually
    // reaches. Recording the far side instead would move every shadow back by
    // the caster's own thickness and drop a caster that has no far side.
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    // Same winding convention as the mesh pass: the Vulkan backend flips the
    // viewport, which flips the winding the rasterizer perceives.
    pipelineDesc.renderState.rasterState.frontCounterClockwise = true;
    // Explicit rather than left at the default, because the default's meaning
    // depends on whether the device has EXT_depth_clip_enable. Clipping is what
    // the cascade fit assumes: it pulls each near plane back to the casters
    // precisely so nothing needs clamping to survive.
    pipelineDesc.renderState.rasterState.depthClipEnable = true;
    // Slope-scaled only. The constant half of the bias is applied per view at
    // sample time, where it can be scaled by that view's texel size — which one
    // rasterizer state, shared by every view, cannot do.
    pipelineDesc.renderState.rasterState.slopeScaledDepthBias = slopeBias;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;

    nvrhi::GraphicsPipelineHandle pipeline = _device->createGraphicsPipeline(pipelineDesc,
                                                                            prototype->getFramebufferInfo());
    if (pipeline == nullptr)
    {
        Core::Log::Error("ShadowDepthRenderer: failed to create the depth-only pipeline.");
    }
    return pipeline;
}

nvrhi::IBindingSet *ShadowDepthRenderer::GetOrCreateBindingSet(nvrhi::IBuffer *instanceBuffer) const
{
    if (_bindingSet != nullptr && _bindingSetInstanceBuffer == instanceBuffer)
    {
        return _bindingSet;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::mat4)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instanceBuffer));
    _bindingSet = _device->createBindingSet(desc, _bindingLayout);
    _bindingSetInstanceBuffer = instanceBuffer;
    return _bindingSet;
}

void ShadowDepthRenderer::EnsureIndirectCapacity(std::uint32_t commandCount) const
{
    if (_indirectBuffer != nullptr && commandCount <= _indirectCapacity)
    {
        return;
    }
    const std::uint32_t grown = std::max(_indirectCapacity * 2u, kInitialCasterCapacity);
    const std::uint32_t capacity = std::max(grown, commandCount);

    nvrhi::BufferDesc desc;
    desc.byteSize = static_cast<std::uint64_t>(sizeof(nvrhi::DrawIndexedIndirectArguments)) * capacity;
    desc.isDrawIndirectArgs = true;
    desc.initialState = nvrhi::ResourceStates::IndirectArgument;
    desc.keepInitialState = true;
    desc.debugName = "ShadowDepthRenderer::IndirectArgs";
    _indirectBuffer = _device->createBuffer(desc);
    _indirectCapacity = capacity;
}

void ShadowDepthRenderer::BeginFrame()
{
    _views.clear();
}

ShadowDepthRenderer::Stats ShadowDepthRenderer::Render(nvrhi::ICommandList *commandList,
                                                       nvrhi::IGraphicsPipeline *pipeline,
                                                       std::span<const ShadowDepthTarget> targets,
                                                       std::span<const ShadowCaster> casters) const
{
    Stats stats;
    stats.firstView = static_cast<std::uint32_t>(_views.size());
    if (!IsReady() || commandList == nullptr || pipeline == nullptr || targets.empty())
    {
        return stats;
    }

    stats.views = static_cast<std::uint32_t>(targets.size());

    // The table describes the frame, not this call: appended to and re-uploaded
    // whole, so it is complete after whichever kind of shadow map draws last.
    for (const ShadowDepthTarget &target : targets)
    {
        _views.push_back(PackShadowView(target.view));
    }
    if (!_viewTable.IsValid() || _views.size() > _viewTable.CapacityElements())
    {
        _viewTable.Create(_device, sizeof(ShadowViewGpu), static_cast<std::uint32_t>(_views.size()),
                          /*allowUnorderedAccess=*/ false, "ShadowDepthRenderer::ViewTable");
    }
    _viewTable.Upload(commandList, _views.data(), static_cast<std::uint32_t>(_views.size()));

    ShadowDrawList &drawList = _drawList;
    BuildShadowDrawList(targets, casters, drawList);

    stats.instances = static_cast<std::uint32_t>(drawList.instances.size());
    stats.batches = static_cast<std::uint32_t>(drawList.commands.size());
    stats.culled = drawList.culled;

    if (drawList.instances.empty())
    {
        return stats;
    }

    if (!_instanceBuffer.IsValid() || drawList.instances.size() > _instanceBuffer.CapacityElements())
    {
        const auto needed = static_cast<std::uint32_t>(drawList.instances.size());
        const std::uint32_t grown = std::max(_instanceBuffer.CapacityElements() * 2u, kInitialCasterCapacity);
        _instanceBuffer.Create(_device, sizeof(ShadowInstanceData), std::max(grown, needed),
                               /*allowUnorderedAccess=*/ false, "ShadowDepthRenderer::Instances");
    }
    EnsureIndirectCapacity(static_cast<std::uint32_t>(drawList.commands.size()));

    _instanceBuffer.Upload(commandList, drawList.instances.data(),
                           static_cast<std::uint32_t>(drawList.instances.size()));
    commandList->writeBuffer(_indirectBuffer, drawList.commands.data(),
                             drawList.commands.size() * sizeof(nvrhi::DrawIndexedIndirectArguments));

    nvrhi::IBindingSet *const bindingSet = GetOrCreateBindingSet(_instanceBuffer.NativeBuffer());

    for (std::uint32_t index = 0; index < targets.size(); ++index)
    {
        const std::uint32_t first = drawList.viewCommandStart[index];
        const std::uint32_t last = drawList.viewCommandStart[index + 1u];
        if (first == last || targets[index].framebuffer == nullptr)
        {
            continue; // nothing survived this view's cull, or it has nowhere to draw
        }

        const nvrhi::Viewport viewport = ShadowViewViewport(targets[index].view);

        // One multi-draw per run of commands sharing an arena's buffers. Every
        // mesh lives in one arena today, so this is a single call per view.
        std::uint32_t runStart = first;
        while (runStart < last)
        {
            nvrhi::IBuffer *const vertexBuffer = drawList.commandVertexBuffers[runStart];
            nvrhi::IBuffer *const indexBuffer = drawList.commandIndexBuffers[runStart];
            std::uint32_t runEnd = runStart + 1u;
            while (runEnd < last && drawList.commandVertexBuffers[runEnd] == vertexBuffer &&
                   drawList.commandIndexBuffers[runEnd] == indexBuffer)
            {
                ++runEnd;
            }

            nvrhi::GraphicsState state;
            state.pipeline = pipeline;
            state.framebuffer = targets[index].framebuffer;
            state.addBindingSet(bindingSet);
            state.viewport.addViewportAndScissorRect(viewport);
            state.addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer, 0, 0});
            state.indexBuffer = nvrhi::IndexBufferBinding{indexBuffer, nvrhi::Format::R32_UINT, 0};
            state.indirectParams = _indirectBuffer;
            commandList->setGraphicsState(state);

            const glm::mat4 lightViewProjection = targets[index].view.viewProjection;
            commandList->setPushConstants(&lightViewProjection, sizeof(lightViewProjection));

            commandList->drawIndexedIndirect(runStart * sizeof(nvrhi::DrawIndexedIndirectArguments),
                                             runEnd - runStart);
            ++stats.drawCalls;

            runStart = runEnd;
        }
    }

    return stats;
}

} // namespace Assisi::Render
