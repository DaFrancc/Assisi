/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ShadowPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/GpuMarker.hpp>
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

nvrhi::Format DepthFormat(ShadowMapFormat format)
{
    return format == ShadowMapFormat::D16 ? nvrhi::Format::D16 : nvrhi::Format::D32;
}
} // namespace

bool ShadowPass::Initialize(const InitParams &params)
{
    _device = params.device;

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

    // t0 = per-instance world matrices, plus the cascade matrix as a push
    // constant. Nothing else: no material, no lights, no frame constants.
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::mat4)));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    _bindingLayout = _device->createBindingLayout(bindingLayoutDesc);
    if (_bindingLayout == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to create the binding layout.");
        return false;
    }

    return CreatePlaceholder();
}

bool ShadowPass::CreatePlaceholder()
{
    nvrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.arraySize = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = nvrhi::Format::D32;
    desc.isShaderResource = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "ShadowPass::NoCascades";
    _placeholderTexture = _device->createTexture(desc);
    if (_placeholderTexture == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to create the placeholder cascade texture.");
        return false;
    }
    _cascadeTexture = _placeholderTexture;
    return true;
}

void ShadowPass::ReleaseTargets()
{
    _cascadeFramebuffers.clear();
    _cascadeTexture = _placeholderTexture;
    _builtCascades = 0;
    _builtResolution = 0;
}

bool ShadowPass::RebuildTargets()
{
    _cascadeFramebuffers.clear();

    nvrhi::TextureDesc desc;
    desc.width = _settings.resolution;
    desc.height = _settings.resolution;
    desc.arraySize = _settings.cascadeCount;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = DepthFormat(_settings.format);
    desc.isRenderTarget = true;
    desc.isShaderResource = true;
    // Written as depth every frame and read as an SRV in the same frame; nvrhi's
    // automatic barriers move it between the two and restore this on the way out.
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    desc.keepInitialState = true;
    desc.debugName = "ShadowPass::Cascades";

    nvrhi::TextureHandle texture = _device->createTexture(desc);
    if (texture == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to allocate the {}x{} x{} cascade array.", _settings.resolution,
                         _settings.resolution, _settings.cascadeCount);
        return false;
    }

    // One framebuffer per slice rather than one layered framebuffer: each
    // cascade is drawn with its own matrix, so there is nothing for a
    // layered pass to amortise without a geometry stage this does not have.
    _cascadeFramebuffers.reserve(_settings.cascadeCount);
    for (std::uint32_t i = 0; i < _settings.cascadeCount; ++i)
    {
        nvrhi::FramebufferDesc framebufferDesc;
        framebufferDesc.setDepthAttachment(
            nvrhi::FramebufferAttachment().setTexture(texture).setArraySlice(static_cast<nvrhi::ArraySlice>(i)));
        nvrhi::FramebufferHandle framebuffer = _device->createFramebuffer(framebufferDesc);
        if (framebuffer == nullptr)
        {
            Core::Log::Error("ShadowPass: failed to create the framebuffer for cascade {}.", i);
            _cascadeFramebuffers.clear();
            return false;
        }
        _cascadeFramebuffers.push_back(std::move(framebuffer));
    }

    _cascadeTexture = std::move(texture);
    _builtCascades = _settings.cascadeCount;
    _builtResolution = _settings.resolution;
    _builtFormat = _settings.format;
    return true;
}

bool ShadowPass::RebuildPipeline()
{
    if (_cascadeFramebuffers.empty())
    {
        return false;
    }

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.inputLayout = _inputLayout;
    pipelineDesc.VS = _vertexShader;
    // No PS: nothing but depth is written, so there is no fragment stage at all.
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode =
        _settings.cullFrontFaces ? nvrhi::RasterCullMode::Front : nvrhi::RasterCullMode::Back;
    // Same winding convention as the mesh pass: the Vulkan backend flips the
    // viewport, which flips the winding the rasterizer perceives.
    pipelineDesc.renderState.rasterState.frontCounterClockwise = true;
    // Explicit rather than left at the default, because the default's meaning
    // depends on whether the device has EXT_depth_clip_enable. Clipping is what
    // the cascade fit assumes: it pulls each near plane back to the casters
    // precisely so nothing needs clamping to survive.
    pipelineDesc.renderState.rasterState.depthClipEnable = true;
    // Slope-scaled only. The constant half of the bias is applied per cascade at
    // sample time, where it can be scaled by that cascade's texel size — which
    // one rasterizer state, shared by all four, cannot do.
    pipelineDesc.renderState.rasterState.slopeScaledDepthBias = _settings.slopeBias;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;

    _pipeline = _device->createGraphicsPipeline(pipelineDesc, _cascadeFramebuffers.front()->getFramebufferInfo());
    if (_pipeline == nullptr)
    {
        Core::Log::Error("ShadowPass: failed to create the depth-only pipeline.");
        return false;
    }
    _builtSlopeBias = _settings.slopeBias;
    _builtCullFrontFaces = _settings.cullFrontFaces;
    return true;
}

bool ShadowPass::Configure(const ShadowSettings &settings, bool active)
{
    if (_device == nullptr || !_vertexShader)
    {
        return false; // Initialize failed; the pass stays inactive for good
    }

    if (!active)
    {
        if (_active)
        {
            // Nothing wants shadows any more: give the memory back rather than
            // holding a 4-cascade array against a scene with no sun in it.
            ReleaseTargets();
            _pipeline = nullptr;
            _bindingSet = nullptr;
            _bindingSetInstanceBuffer = nullptr;
            _active = false;
        }
        return true;
    }

    const ShadowSettings safe = Sanitized(settings);
    const bool targetsStale = _cascadeFramebuffers.empty() || safe.cascadeCount != _builtCascades ||
                              safe.resolution != _builtResolution || safe.format != _builtFormat;
    const bool pipelineStale = _pipeline == nullptr || safe.slopeBias != _builtSlopeBias ||
                               safe.cullFrontFaces != _builtCullFrontFaces;
    _settings = safe;

    if (!targetsStale && !pipelineStale)
    {
        _active = true;
        return true;
    }

    if (targetsStale && !RebuildTargets())
    {
        ReleaseTargets();
        _pipeline = nullptr;
        _active = false;
        return false;
    }
    // A new array means new framebuffers, and a pipeline is built against one.
    if ((targetsStale || pipelineStale) && !RebuildPipeline())
    {
        ReleaseTargets();
        _pipeline = nullptr;
        _active = false;
        return false;
    }

    _active = true;
    return true;
}

nvrhi::IBindingSet *ShadowPass::GetOrCreateBindingSet(nvrhi::IBuffer *instanceBuffer) const
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

void ShadowPass::EnsureIndirectCapacity(std::uint32_t commandCount) const
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
    desc.debugName = "ShadowPass::IndirectArgs";
    _indirectBuffer = _device->createBuffer(desc);
    _indirectCapacity = capacity;
}

ShadowPass::Stats ShadowPass::Render(nvrhi::ICommandList *commandList, const CascadeFit &fit,
                                     std::span<const Caster> casters) const
{
    Stats stats;
    if (!IsActive() || commandList == nullptr || fit.count == 0)
    {
        return stats;
    }

    const std::uint32_t cascadeCount = std::min<std::uint32_t>(fit.count, _builtCascades);
    stats.cascades = cascadeCount;

    ASSISI_PROFILE_GPU_PASS(commandList, "shadow-cascades");

    // Build every cascade's records and commands in one pass. All cascades share
    // one instance buffer — each command's startInstanceLocation points into its
    // own cascade's run — so a frame uploads once however many cascades it draws.
    std::vector<InstanceData> &instances = _scratchInstances;
    std::vector<nvrhi::DrawIndexedIndirectArguments> &commands = _scratchCommands;
    std::vector<const MeshBuffer *> &batchMeshes = _scratchBatchMeshes;
    instances.clear();
    commands.clear();
    batchMeshes.clear();

    for (std::uint32_t cascade = 0; cascade < cascadeCount; ++cascade)
    {
        _scratchCascadeCommandStart[cascade] = static_cast<std::uint32_t>(commands.size());

        const Frustum frustum = Frustum::FromViewProjection(fit.cascades[cascade].viewProjection);

        const MeshBuffer *previousMesh = nullptr;
        std::uint32_t previousSubmesh = UINT32_MAX;
        for (const Caster &caster : casters)
        {
            if (caster.mesh == nullptr)
            {
                continue;
            }
            if (!frustum.IntersectsSphere(caster.worldSphere))
            {
                ++stats.culled;
                previousMesh = nullptr; // a gap in the run: the next item opens a new batch
                continue;
            }

            const auto instanceIndex = static_cast<std::uint32_t>(instances.size());
            instances.push_back(InstanceData{caster.model});

            if (caster.mesh == previousMesh && caster.submeshIndex == previousSubmesh)
            {
                ++commands.back().instanceCount;
            }
            else
            {
                const Geometry::SubMesh &subMesh = caster.mesh->SubMeshes()[caster.submeshIndex];
                nvrhi::DrawIndexedIndirectArguments command;
                command.indexCount = subMesh.IndexCount;
                command.instanceCount = 1;
                command.startIndexLocation = caster.mesh->IndexBase() + subMesh.IndexOffset;
                command.baseVertexLocation = static_cast<std::int32_t>(caster.mesh->VertexBase());
                command.startInstanceLocation = instanceIndex;
                commands.push_back(command);
                batchMeshes.push_back(caster.mesh);
                previousMesh = caster.mesh;
                previousSubmesh = caster.submeshIndex;
            }
        }
    }
    _scratchCascadeCommandStart[cascadeCount] = static_cast<std::uint32_t>(commands.size());

    stats.instances = static_cast<std::uint32_t>(instances.size());
    stats.batches = static_cast<std::uint32_t>(commands.size());

    if (!instances.empty())
    {
        if (!_instanceBuffer.IsValid() || instances.size() > _instanceBuffer.CapacityElements())
        {
            const auto needed = static_cast<std::uint32_t>(instances.size());
            const std::uint32_t grown = std::max(_instanceBuffer.CapacityElements() * 2u, kInitialCasterCapacity);
            _instanceBuffer.Create(_device, sizeof(InstanceData), std::max(grown, needed),
                                   /*allowUnorderedAccess=*/ false, "ShadowPass::Instances");
        }
        EnsureIndirectCapacity(static_cast<std::uint32_t>(commands.size()));

        _instanceBuffer.Upload(commandList, instances.data(), static_cast<std::uint32_t>(instances.size()));
        commandList->writeBuffer(_indirectBuffer, commands.data(),
                                 commands.size() * sizeof(nvrhi::DrawIndexedIndirectArguments));
    }

    for (std::uint32_t cascade = 0; cascade < cascadeCount; ++cascade)
    {
        // Cleared whether or not anything draws into it: a stale slice would
        // shadow this frame with last frame's geometry.
        commandList->clearDepthStencilTexture(
            _cascadeTexture, nvrhi::TextureSubresourceSet(0, 1, static_cast<nvrhi::ArraySlice>(cascade), 1), true, 1.0f,
            false, 0);

        const std::uint32_t first = _scratchCascadeCommandStart[cascade];
        const std::uint32_t last = _scratchCascadeCommandStart[cascade + 1];
        if (first == last)
        {
            continue; // nothing survived this cascade's cull
        }

        nvrhi::IBindingSet *const bindingSet = GetOrCreateBindingSet(_instanceBuffer.NativeBuffer());

        // One multi-draw per run of batches sharing an arena's buffers. Stage C
        // keeps every mesh in one arena, so this is a single call per cascade.
        std::uint32_t runStart = first;
        while (runStart < last)
        {
            nvrhi::IBuffer *const vertexBuffer = batchMeshes[runStart]->VertexBuffer();
            nvrhi::IBuffer *const indexBuffer = batchMeshes[runStart]->IndexBuffer();
            std::uint32_t runEnd = runStart + 1;
            while (runEnd < last && batchMeshes[runEnd]->VertexBuffer() == vertexBuffer &&
                   batchMeshes[runEnd]->IndexBuffer() == indexBuffer)
            {
                ++runEnd;
            }

            nvrhi::GraphicsState state;
            state.pipeline = _pipeline;
            state.framebuffer = _cascadeFramebuffers[cascade];
            state.addBindingSet(bindingSet);
            state.viewport.addViewportAndScissorRect(
                nvrhi::Viewport(static_cast<float>(_settings.resolution), static_cast<float>(_settings.resolution)));
            state.addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer, 0, 0});
            state.indexBuffer = nvrhi::IndexBufferBinding{indexBuffer, nvrhi::Format::R32_UINT, 0};
            state.indirectParams = _indirectBuffer;
            commandList->setGraphicsState(state);

            const glm::mat4 lightViewProjection = fit.cascades[cascade].viewProjection;
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
