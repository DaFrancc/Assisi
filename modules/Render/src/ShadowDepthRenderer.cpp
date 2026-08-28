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

/// @brief Apply the caster-side depth bias to @p state.
///
/// The slope-scaled term is the one that matters: it is the polygon's own depth
/// slope per pixel, which is format-independent and is what a surface tilted
/// away from the light needs to stop shadowing itself.
///
/// The constant term has to be non-zero anyway, and that is not a tuning
/// decision. Vulkan applies no bias at all unless depthBiasEnable is set, and
/// nvrhi derives that flag from this field alone — a pipeline that sets only
/// the slope factor gets neither. One unit is the smallest value that turns the
/// state on: the spec scales it by an implementation-defined minimum resolvable
/// difference, so a larger number would mean different things on different
/// devices and formats, and anything that has to be portable belongs in the
/// biases the sample side computes in units it knows.
constexpr int kEnablingConstantBias = 1;

void ApplyDepthBias(nvrhi::RasterState &state, float slopeBias, float slopeBiasClamp)
{
    state.depthBias = kEnablingConstantBias;
    state.slopeScaledDepthBias = slopeBias;
    // Vulkan takes this as a depth, not as a multiple of anything, and it is the
    // largest gap the caster side can open under a silhouette. The caller sizes
    // it against the map's texel so it shrinks when the texels do.
    state.depthBiasClamp = slopeBiasClamp;
}
} // namespace

void ShadowDrawList::Clear()
{
    instances.clear();
    commands.clear();
    commandVertexBuffers.clear();
    commandIndexBuffers.clear();
    viewCommandStart.clear();
    viewMaskedStart.clear();
    culled = 0;
}

namespace
{
/// @brief Cull one view's casters of a single alpha class and append their
/// commands, coalescing consecutive runs of the same geometry.
///
/// One class per sweep is what puts a view's commands in pipeline order without
/// the caller having had to sort the span: the sweep that wants the other class
/// rejects on the flag before the frustum test, so a caster is only ever culled
/// — and only ever counted as culled — by the sweep that would have drawn it.
void AppendViewCommands(const Frustum &frustum, std::span<const ShadowCaster> casters, bool alphaMasked,
                        ShadowDrawList &out)
{
    const ShadowCaster *run = nullptr;
    for (const ShadowCaster &caster : casters)
    {
        if (caster.alphaMasked != alphaMasked || caster.indexCount == 0)
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
        out.instances.push_back(ShadowInstanceData{.model = caster.model, .materialIndex = caster.materialIndex});

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
} // namespace

void BuildShadowDrawList(std::span<const ShadowDepthTarget> targets, std::span<const ShadowCaster> casters,
                         ShadowDrawList &out)
{
    out.Clear();
    out.viewCommandStart.reserve(targets.size() + 1u);
    out.viewMaskedStart.reserve(targets.size());

    // One pass over the whole span, not one per view: a scene with no cutout in
    // it — which is every scene until content has one — then skips the second
    // sweep entirely and does exactly the work it did before this existed.
    const bool anyMasked =
        std::any_of(casters.begin(), casters.end(), [](const ShadowCaster &caster) { return caster.alphaMasked; });

    for (const ShadowDepthTarget &target : targets)
    {
        out.viewCommandStart.push_back(static_cast<std::uint32_t>(out.commands.size()));

        const Frustum frustum = Frustum::FromViewProjection(target.view.viewProjection);
        AppendViewCommands(frustum, casters, /*alphaMasked=*/ false, out);

        out.viewMaskedStart.push_back(static_cast<std::uint32_t>(out.commands.size()));
        if (anyMasked)
        {
            AppendViewCommands(frustum, casters, /*alphaMasked=*/ true, out);
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

    InitializeAlphaTest(params);
    return true;
}

void ShadowDepthRenderer::InitializeAlphaTest(const InitParams &params)
{
    // Every failure below leaves the renderer usable with alpha-tested casters
    // drawing solid, which is why none of them returns false: losing the hole in
    // a shadow is a worse picture, but no shadows at all is a worse one still.
    if (params.maskedVertexShaderSpvPath.empty() || params.maskedPixelShaderSpvPath.empty() ||
        params.materialTable == nullptr || params.bindlessLayout == nullptr || params.bindlessTable == nullptr)
    {
        return;
    }

    _maskedVertexShader = LoadSpirvShader(_device, params.maskedVertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _maskedPixelShader = LoadSpirvShader(_device, params.maskedPixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_maskedVertexShader || !_maskedPixelShader)
    {
        Core::Log::Warn("ShadowDepthRenderer: alpha-tested casters will cast solid (the variant failed to load).");
        _maskedVertexShader = nullptr;
        _maskedPixelShader = nullptr;
        return;
    }

    // Position and the texture coordinate the alpha is sampled at. Still no
    // normal or tangent: nothing here shades.
    using Assisi::Geometry::Vertex;
    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
        .setName("POSITION")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(Vertex, Position))
        .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
        .setName("TEXCOORD")
        .setFormat(nvrhi::Format::RG32_FLOAT)
        .setOffset(offsetof(Vertex, TextureCoordinates))
        .setElementStride(sizeof(Vertex)),
    };
    _maskedInputLayout = _device->createInputLayout(attributes, static_cast<std::uint32_t>(std::size(attributes)),
                                                    _maskedVertexShader);

    // t0 = instances (vertex), t1 = the material table and s0 the sampler its
    // base-colour slot is read through (fragment), plus the same view push
    // constant. The bindless textures join as register space 1.
    nvrhi::BindingLayoutDesc maskedLayoutDesc;
    maskedLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    maskedLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::mat4)));
    maskedLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    maskedLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    maskedLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _maskedBindingLayout = _device->createBindingLayout(maskedLayoutDesc);

    // Repeat and trilinear, matching the mesh pass: the alpha the shadow tests
    // has to be the alpha the surface shows, or the hole moves between them.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Repeat);
    _maskedSampler = _device->createSampler(samplerDesc);

    if (_maskedInputLayout == nullptr || _maskedBindingLayout == nullptr || _maskedSampler == nullptr)
    {
        Core::Log::Warn("ShadowDepthRenderer: alpha-tested casters will cast solid (a layout failed to build).");
        _maskedVertexShader = nullptr;
        _maskedPixelShader = nullptr;
        return;
    }

    _materialTable = params.materialTable;
    _bindlessLayout = params.bindlessLayout;
    _bindlessTable = params.bindlessTable;
}

bool ShadowDepthRenderer::CanAlphaTest() const
{
    return IsReady() && _maskedPixelShader != nullptr && _materialTable != nullptr;
}

nvrhi::GraphicsPipelineHandle ShadowDepthRenderer::CreatePipeline(nvrhi::IFramebuffer *prototype, float slopeBias,
                                                                  float slopeBiasClamp) const
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
    ApplyDepthBias(pipelineDesc.renderState.rasterState, slopeBias, slopeBiasClamp);
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

nvrhi::GraphicsPipelineHandle ShadowDepthRenderer::CreateMaskedPipeline(nvrhi::IFramebuffer *prototype,
                                                                        float slopeBias, float slopeBiasClamp) const
{
    if (!CanAlphaTest() || prototype == nullptr)
    {
        return nullptr;
    }

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.inputLayout = _maskedInputLayout;
    pipelineDesc.VS = _maskedVertexShader;
    pipelineDesc.PS = _maskedPixelShader;
    pipelineDesc.addBindingLayout(_maskedBindingLayout);
    pipelineDesc.addBindingLayout(_bindlessLayout);
    // Both faces, where the opaque pipeline keeps the back one. Culling at all
    // assumes a closed shell, and a cutout is characteristically a thin card
    // whose front and back are one coincident surface — cull either and the
    // caster disappears whenever its winding faces the wrong way for the light.
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.rasterState.frontCounterClockwise = true;
    pipelineDesc.renderState.rasterState.depthClipEnable = true;
    ApplyDepthBias(pipelineDesc.renderState.rasterState, slopeBias, slopeBiasClamp);
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;

    nvrhi::GraphicsPipelineHandle pipeline = _device->createGraphicsPipeline(pipelineDesc,
                                                                             prototype->getFramebufferInfo());
    if (pipeline == nullptr)
    {
        Core::Log::Error("ShadowDepthRenderer: failed to create the alpha-testing depth pipeline.");
    }
    return pipeline;
}

nvrhi::IBindingSet *ShadowDepthRenderer::GetOrCreateMaskedBindingSet(nvrhi::IBuffer *instanceBuffer) const
{
    if (!CanAlphaTest())
    {
        return nullptr;
    }
    if (_maskedBindingSet != nullptr && _maskedBindingSetInstanceBuffer == instanceBuffer)
    {
        return _maskedBindingSet;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::mat4)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, _materialTable));
    desc.addItem(nvrhi::BindingSetItem::Sampler(0, _maskedSampler));
    _maskedBindingSet = _device->createBindingSet(desc, _maskedBindingLayout);
    _maskedBindingSetInstanceBuffer = instanceBuffer;
    return _maskedBindingSet;
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
                                                       const ShadowPipelines &pipelines,
                                                       std::span<const ShadowDepthTarget> targets,
                                                       std::span<const ShadowCaster> casters) const
{
    Stats stats;
    stats.firstView = static_cast<std::uint32_t>(_views.size());
    if (!IsReady() || commandList == nullptr || pipelines.opaque == nullptr || targets.empty())
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
    nvrhi::IBindingSet *const maskedBindingSet = GetOrCreateMaskedBindingSet(_instanceBuffer.NativeBuffer());
    // Without a pipeline that can discard, or a set to feed it, the cutouts draw
    // through the opaque one: a solid silhouette rather than no shadow at all.
    const bool alphaTesting = pipelines.masked != nullptr && maskedBindingSet != nullptr;

    for (std::uint32_t index = 0; index < targets.size(); ++index)
    {
        if (targets[index].framebuffer == nullptr)
        {
            continue; // this view has nowhere to draw
        }

        const std::uint32_t viewEnd = drawList.viewCommandStart[index + 1u];
        const std::uint32_t maskedStart = alphaTesting ? drawList.viewMaskedStart[index] : viewEnd;
        stats.maskedBatches += viewEnd - maskedStart;

        const nvrhi::Viewport viewport = ShadowViewViewport(targets[index].view);

        // One multi-draw per run of commands sharing an arena's buffers, and a
        // break at the alpha-test boundary because the two halves carry
        // different pipelines. Every mesh lives in one arena today, so an
        // all-opaque view is a single call.
        std::uint32_t runStart = drawList.viewCommandStart[index];
        while (runStart < viewEnd)
        {
            const bool masked = runStart >= maskedStart;
            const std::uint32_t halfEnd = masked ? viewEnd : maskedStart;

            nvrhi::IBuffer *const vertexBuffer = drawList.commandVertexBuffers[runStart];
            nvrhi::IBuffer *const indexBuffer = drawList.commandIndexBuffers[runStart];
            std::uint32_t runEnd = runStart + 1u;
            while (runEnd < halfEnd && drawList.commandVertexBuffers[runEnd] == vertexBuffer &&
                   drawList.commandIndexBuffers[runEnd] == indexBuffer)
            {
                ++runEnd;
            }

            nvrhi::GraphicsState state;
            state.pipeline = masked ? pipelines.masked : pipelines.opaque;
            state.framebuffer = targets[index].framebuffer;
            state.addBindingSet(masked ? maskedBindingSet : bindingSet);
            if (masked)
            {
                state.addBindingSet(_bindlessTable);
            }
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
