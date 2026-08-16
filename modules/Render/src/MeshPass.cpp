/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshPass.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

namespace Assisi::Render
{

namespace
{
// Per-instance data: one record per drawn submesh, uploaded to the instance
// buffer each frame and indexed in the vertex shader by gl_InstanceIndex (each
// draw sets startInstanceLocation to its record). Mirrors mesh.vert's
// `InstanceData` std430 struct — mat4 (0..63) + uint (64), padded to the 16-byte
// array stride std430 gives the struct.
// (Declared as MeshPass::InstanceData in the header so the pass can hold a
// reusable scratch vector of them across frames.)
using InstanceData = MeshPass::InstanceData;

// Starting instance-buffer capacity (records). Grows geometrically past this when
// a frame draws more items; the first level typically fits without a growth. Also
// the starting capacity for the indirect-args buffer (batches <= instances).
constexpr uint32_t kInitialInstanceCapacity = 1024u;

// The indirect command matches Vulkan's VkDrawIndexedIndirectCommand byte-for-byte
// (five tightly-packed 32-bit fields), so the batch vector uploads straight into
// the indirect-args buffer with no repack.
static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 20,
              "DrawIndexedIndirectArguments must match VkDrawIndexedIndirectCommand's packed layout.");

// Per-frame data (view-projection + camera + cluster-grid parameters), uploaded
// once per frame via UpdateFrameConstants() into a constant buffer. viewProjection
// leads so the vertex shader can form clip position from each instance's world
// matrix. Mirrors mesh.vert/frag's matching `uniform FrameConstants` block.
struct FrameConstants
{
    glm::mat4 viewProjection;
    glm::mat4 view;
    glm::uvec4 gridDim;           // xyz used, w unused
    glm::vec4 screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    glm::uvec4 lightCounts;       // x = directional light count, y = debug view, zw unused
    /// World-space camera position, w unused. Derived once here rather than per
    /// fragment: it is constant across the frame, but view is a uniform, so the
    /// shader compiler cannot hoist the -transpose(mat3(view)) * view[3] out.
    glm::vec4 cameraPosition;
    /// Froxel lookup scale/bias, so mesh.frag's ClusterIndex() is FMAs and a
    /// single log instead of three divides and two logs (the Doom-2016 form):
    ///   xy = gridDim.xy / screenSize        (screen pixel -> cluster column/row)
    ///   z  = gridDim.z / log(farZ / nearZ)  (log-depth slice scale)
    ///   w  = -z * log(nearZ)                (matching bias)
    /// slice = log(|viewZ|) * z + w, which is gridDim.z * log(|viewZ|/nearZ) / log(farZ/nearZ).
    glm::vec4 clusterScale;
    /// Uniform ambient term: rgb = linear colour, w = intensity. A frame constant
    /// rather than a shader constant so a scene can be turned up for inspection;
    /// the default is white × kDefaultAmbientIntensity.
    glm::vec4 ambient;
};
} // namespace

bool MeshPass::Initialize(const InitParams &params)
{
    nvrhi::IDevice *const device = params.device;
    const nvrhi::FramebufferInfo &framebufferInfo = params.framebufferInfo;
    const std::string &vertexShaderSpvPath = params.vertexShaderSpvPath;
    const std::string &pixelShaderSpvPath = params.pixelShaderSpvPath;

    _device = device;
    _clusterGrid = params.clusterGrid;
    _bindlessLayout = params.bindlessLayout;
    _bindlessTable = params.bindlessTable;
    _materialTable = params.materialTable;

    _vertexShader = LoadSpirvShader(device, vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader = LoadSpirvShader(device, pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        return false;
    }

    using Assisi::Geometry::Vertex;
    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
        .setName("POSITION")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(Vertex, Position))
        .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
        .setName("NORMAL")
        .setFormat(nvrhi::Format::RGB32_FLOAT)
        .setOffset(offsetof(Vertex, Normal))
        .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
        .setName("TEXCOORD")
        .setFormat(nvrhi::Format::RG32_FLOAT)
        .setOffset(offsetof(Vertex, TextureCoordinates))
        .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
        .setName("TANGENT")
        .setFormat(nvrhi::Format::RGBA32_FLOAT)
        .setOffset(offsetof(Vertex, Tangent))
        .setElementStride(sizeof(Vertex)),
    };
    _inputLayout = device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), _vertexShader);

    // Set 0 — the one binding set every draw shares (stage D). Texture_SRV/Sampler
    // are separate descriptors in NVRHI's Vulkan backend (HLSL t/s split, not
    // GLSL's combined sampler2D); StructuredBuffer_SRV shares the t-register space
    // with Texture_SRV. Slot map (see mesh.vert/frag's matching `binding = N`):
    //   b0 = FrameConstants (no more per-material CB — the factors live in t0)
    //   t0 = material table, t1-t5 = clustered light buffers, t6 = per-instance data
    //   s0 = shared sampler
    // No push constants: per-object data (world matrix + material id) is read from
    // the instance buffer (t6) by gl_InstanceIndex, and the material's textures
    // from the bindless table (register space 1).
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));       // FrameConstants
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));              // shared sampler
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)); // material table
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1)); // pointLights
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2)); // spotLights
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3)); // dirLights
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4)); // lightIndexList
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5)); // lightGrid
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6)); // per-instance data
    _bindingLayout = device->createBindingLayout(bindingLayoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true); // trilinear: linear min/mag + linear mip blending
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Repeat);
    // Anisotropic filtering keeps textures sharp at grazing angles, where plain
    // trilinear picks an over-coarse mip and washes the surface out. The context
    // reports 1.0 (isotropic) when the device didn't enable the feature, so this
    // is a no-op there rather than an invalid request.
    if (const Vulkan::VulkanContext *context = RenderSystem::GetVulkanContext())
    {
        samplerDesc.maxAnisotropy = context->GetMaxAnisotropy();
    }
    _sampler = device->createSampler(samplerDesc);

    nvrhi::BufferDesc frameConstantsDesc;
    frameConstantsDesc.byteSize = sizeof(FrameConstants);
    frameConstantsDesc.isConstantBuffer = true;
    frameConstantsDesc.debugName = "MeshPass::FrameConstants";
    frameConstantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    frameConstantsDesc.keepInitialState = true;
    _frameConstantsBuffer = device->createBuffer(frameConstantsDesc);
    if (_frameConstantsBuffer == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the frame-constants buffer.");
        return false;
    }

    return RebuildPipeline(framebufferInfo);
}

bool MeshPass::RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.inputLayout = _inputLayout;
    pipelineDesc.VS = _vertexShader;
    pipelineDesc.PS = _pixelShader;
    pipelineDesc.addBindingLayout(_bindingLayout);  // set 0: CBs, sampler, light buffers
    pipelineDesc.addBindingLayout(_bindlessLayout);  // set 1: bindless material textures
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    // Meshes are authored CCW-front (standard convention). NVRHI's Vulkan backend
    // flips the viewport (VKViewportWithDXCoords) to undo Vulkan's native Y-down
    // clip space, which also flips the winding order the rasterizer perceives —
    // without this, back-face culling culls the actual front faces instead.
    pipelineDesc.renderState.rasterState.frontCounterClockwise = true;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;

    _pipeline = _device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (_pipeline == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the graphics pipeline.");
        return false;
    }
    return true;
}

void MeshPass::UpdateFrameConstants(nvrhi::ICommandList *commandList, const glm::mat4 &viewProjection,
                                    const glm::mat4 &view, uint32_t screenWidth, uint32_t screenHeight, float nearZ,
                                    float farZ, uint32_t dirLightCount, MaterialDebugView debugView,
                                    const glm::vec3 &ambientColor, float ambientIntensity) const
{
    FrameConstants constants;
    constants.viewProjection = viewProjection;
    constants.view = view;
    constants.gridDim = glm::uvec4(ClusterGrid::kNumX, ClusterGrid::kNumY, ClusterGrid::kNumZ, 0u);
    constants.screenSizeNearFar =
        glm::vec4(static_cast<float>(screenWidth), static_cast<float>(screenHeight), nearZ, farZ);
    constants.lightCounts = glm::uvec4(dirLightCount, static_cast<uint32_t>(debugView), 0u, 0u);

    // View is a rigid transform (View = R | t, t = -R * cameraPos), so the camera
    // position is -R^-1 * t, and R^-1 == transpose(R) because R is orthonormal.
    constants.cameraPosition = glm::vec4(-glm::transpose(glm::mat3(view)) * glm::vec3(view[3]), 0.f);

    // Guard the logs: a zero/negative near or far plane would make these inf/NaN
    // and poison every cluster lookup. Both come from the camera, which validates
    // them, so this is belt-and-braces rather than an expected path.
    const float safeNear = nearZ > 0.f ? nearZ : 0.001f;
    const float safeFar  = farZ > safeNear ? farZ : safeNear * 2.f;
    const float logRatio = std::log(safeFar / safeNear);
    const float sliceScale = static_cast<float>(ClusterGrid::kNumZ) / logRatio;
    constants.clusterScale =
        glm::vec4(static_cast<float>(ClusterGrid::kNumX) / static_cast<float>(screenWidth),
                  static_cast<float>(ClusterGrid::kNumY) / static_cast<float>(screenHeight), sliceScale,
                  -sliceScale * std::log(safeNear));

    constants.ambient = glm::vec4(ambientColor, ambientIntensity);

    commandList->writeBuffer(_frameConstantsBuffer, &constants, sizeof(constants));
}

nvrhi::IBindingSet *MeshPass::GetOrCreateGlobalBindingSet(nvrhi::IBuffer *instanceBuffer) const
{
    if (_globalBindingSet != nullptr && _globalSetInstanceBuffer == instanceBuffer)
    {
        return _globalBindingSet;
    }

    // Every referenced handle but the instance buffer is stable for the pass's
    // lifetime (frame CB / sampler owned here; the light buffers, though rebuilt
    // on a viewport change, keep their handles; the material table is fixed
    // capacity). So this rebuilds only on an instance-buffer growth or an explicit
    // InvalidateBindingSets() — not per frame.
    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _frameConstantsBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, _materialTable));
    bindingSetDesc.addItem(
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, _clusterGrid->PointLightBuffer().NativeBuffer()));
    bindingSetDesc.addItem(
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, _clusterGrid->SpotLightBuffer().NativeBuffer()));
    bindingSetDesc.addItem(
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, _clusterGrid->DirLightBuffer().NativeBuffer()));
    bindingSetDesc.addItem(
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, _clusterGrid->LightIndexBuffer().NativeBuffer()));
    bindingSetDesc.addItem(
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, _clusterGrid->LightGridBuffer().NativeBuffer()));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, instanceBuffer));
    _globalBindingSet = _device->createBindingSet(bindingSetDesc, _bindingLayout);
    _globalSetInstanceBuffer = instanceBuffer;
    return _globalBindingSet;
}

void MeshPass::EnsureIndirectCapacity(uint32_t commandCount) const
{
    if (_indirectBuffer != nullptr && commandCount <= _indirectCapacity)
    {
        return;
    }
    const uint32_t grown = std::max(_indirectCapacity * 2u, kInitialInstanceCapacity);
    const uint32_t capacity = std::max(grown, commandCount);

    nvrhi::BufferDesc desc;
    desc.byteSize = static_cast<uint64_t>(sizeof(nvrhi::DrawIndexedIndirectArguments)) * capacity;
    desc.isDrawIndirectArgs = true;
    desc.initialState = nvrhi::ResourceStates::IndirectArgument;
    desc.keepInitialState = true;
    desc.debugName = "MeshPass::IndirectArgs";
    _indirectBuffer = _device->createBuffer(desc);
    _indirectCapacity = capacity;
}

MeshPass::SubmitStats MeshPass::Submit(const RenderFrame &frame, std::span<const DrawItem> items) const
{
    nvrhi::ICommandList *const commandList = frame.commandList;
    SubmitStats stats;

    // Build the frame's per-instance records and indirect commands in one pass over
    // the (already sorted) items. Each valid item appends one instance record — the
    // vertex shader reads it via gl_InstanceIndex — and either extends the current
    // batch or opens a new one. A batch is a maximal run of consecutive items with
    // the same geometry (mesh + submesh); its records are contiguous (instanceIndex
    // increments per item), so one instanced draw covers them. Material differs per
    // instance (read from the record), so it never splits a batch.
    //
    // Reused members, not locals: clear() keeps the capacity, so a steady-state
    // scene allocates nothing here.
    std::vector<InstanceData> &instances   = _scratchInstances;
    std::vector<nvrhi::DrawIndexedIndirectArguments> &commands    = _scratchCommands;
    // Parallel to `commands`: the mesh each batch draws, for the arena vertex/index
    // buffers at record time (they bind via GraphicsState, not the indirect args).
    std::vector<const MeshBuffer *> &batchMeshes = _scratchBatchMeshes;
    instances.clear();
    commands.clear();
    batchMeshes.clear();
    instances.reserve(items.size());

    const MeshBuffer *prevMesh    = nullptr;
    uint32_t prevSubmesh = UINT32_MAX;
    uint32_t instanceIndex = 0;
    for (const DrawItem &item : items)
    {
        if (item.material == nullptr || item.mesh == nullptr)
        {
            continue; // producer should have filtered these; belt and suspenders
        }
        // The shader indexes `materials[]` (an unbounded SSBO runtime array) with
        // this id, so the GPU cannot bounds-check it — an id past the table reads
        // whatever memory follows. AssetCache::MintMaterialId saturates to keep
        // that from happening; catch a violation here, in debug, before it ships
        // to the GPU where it would be silent.
        ASSISI_ASSERT(item.material->Id() < AssetCache::kMaxMaterials,
                      "material id indexes past the material table — the shader read would be out of bounds");
        instances.push_back(InstanceData{item.model, item.material->Id()});

        if (!commands.empty() && item.mesh == prevMesh && item.submeshIndex == prevSubmesh)
        {
            ++commands.back().instanceCount; // same geometry as the run — coalesce
        }
        else
        {
            // Arena addressing: index values stay mesh-local, so baseVertexLocation
            // shifts them into this mesh's vertex range and startIndexLocation picks
            // its slice of the shared index buffer. startInstanceLocation makes the
            // batch's first gl_InstanceIndex select its first record.
            const Geometry::SubMesh &subMesh = item.mesh->SubMeshes()[item.submeshIndex];
            nvrhi::DrawIndexedIndirectArguments cmd;
            cmd.indexCount            = subMesh.IndexCount;
            cmd.instanceCount         = 1;
            cmd.startIndexLocation    = item.mesh->IndexBase() + subMesh.IndexOffset;
            cmd.baseVertexLocation    = static_cast<int32_t>(item.mesh->VertexBase());
            cmd.startInstanceLocation = instanceIndex;
            commands.push_back(cmd);
            batchMeshes.push_back(item.mesh);
            prevMesh    = item.mesh;
            prevSubmesh = item.submeshIndex;
        }
        ++instanceIndex;
    }
    if (commands.empty())
    {
        return stats; // nothing to draw — leave the instance/indirect buffers untouched
    }
    stats.instances = static_cast<uint32_t>(instances.size());
    stats.batches   = static_cast<uint32_t>(commands.size());

    // Grow the instance buffer to hold this frame's records if needed. A growth
    // swaps the buffer handle, so the cached global set (which references it) must
    // be rebuilt — GetOrCreateGlobalBindingSet notices via _globalSetInstanceBuffer.
    if (!_instanceBuffer.IsValid() || instances.size() > _instanceBuffer.CapacityElements())
    {
        const uint32_t needed = static_cast<uint32_t>(instances.size());
        const uint32_t grown = std::max(_instanceBuffer.CapacityElements() * 2u, kInitialInstanceCapacity);
        _instanceBuffer.Create(_device, sizeof(InstanceData), std::max(grown, needed), /*allowUnorderedAccess=*/ false,
                               "MeshPass::Instances");
    }
    EnsureIndirectCapacity(static_cast<uint32_t>(commands.size()));

    // Upload both buffers before recording draws: the copies land in this command
    // list ahead of the draws that read them, so the ordering is correct in-frame.
    _instanceBuffer.Upload(commandList, instances.data(), static_cast<uint32_t>(instances.size()));
    commandList->writeBuffer(_indirectBuffer, commands.data(),
                             commands.size() * sizeof(nvrhi::DrawIndexedIndirectArguments));

    nvrhi::IBindingSet *const globalBindingSet = GetOrCreateGlobalBindingSet(_instanceBuffer.NativeBuffer());

    // Multi-draw each maximal run of batches that share the same arena vertex/index
    // buffers with one drawIndexedIndirect. Stage C keeps every mesh in one arena,
    // so this is a single call spanning all batches; the per-buffer grouping stays
    // correct if a second arena (divergent vertex format) is ever added.
    const uint32_t commandCount = static_cast<uint32_t>(commands.size());
    uint32_t runStart     = 0;
    while (runStart < commandCount)
    {
        nvrhi::IBuffer *const vertexBuffer = batchMeshes[runStart]->VertexBuffer();
        nvrhi::IBuffer *const indexBuffer  = batchMeshes[runStart]->IndexBuffer();
        uint32_t runEnd       = runStart + 1;
        while (runEnd < commandCount && batchMeshes[runEnd]->VertexBuffer() == vertexBuffer &&
               batchMeshes[runEnd]->IndexBuffer() == indexBuffer)
        {
            ++runEnd;
        }

        nvrhi::GraphicsState state;
        state.pipeline = _pipeline;
        state.framebuffer = frame.framebuffer;
        state.addBindingSet(globalBindingSet); // set 0: frame/sampler/lights/material table/instances
        state.addBindingSet(_bindlessTable);   // set 1: bindless textures
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
        state.addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer, 0, 0});
        state.indexBuffer = nvrhi::IndexBufferBinding{indexBuffer, nvrhi::Format::R32_UINT, 0};
        state.indirectParams = _indirectBuffer;
        commandList->setGraphicsState(state);

        commandList->drawIndexedIndirect(runStart * sizeof(nvrhi::DrawIndexedIndirectArguments), runEnd - runStart);
        ++stats.drawCalls;

        runStart = runEnd;
    }

    return stats;
}

MeshPass::SubmitStats MeshPass::SubmitIndirect(const RenderFrame &frame, const IndirectDrawInputs &in) const
{
    SubmitStats stats;
    if (in.indirectBuffer == nullptr || in.instanceBuffer == nullptr || in.vertexBuffer == nullptr ||
        in.indexBuffer == nullptr || in.commandCount == 0)
    {
        return stats; // nothing culled this frame (or the culler is unavailable)
    }

    nvrhi::ICommandList *const commandList = frame.commandList;

    // Bind the global set against the cull pass's instance buffer (t6) — the
    // records it wrote are read by gl_InstanceIndex just like the CPU path's.
    nvrhi::IBindingSet *const globalBindingSet = GetOrCreateGlobalBindingSet(in.instanceBuffer);

    // One multi-draw over every batch command. Single arena → all batches share the
    // arena's vertex/index buffers (stage C/E), so one drawIndexedIndirect covers
    // them; empty batches carry instanceCount 0 and draw nothing.
    nvrhi::GraphicsState state;
    state.pipeline     = _pipeline;
    state.framebuffer  = frame.framebuffer;
    state.addBindingSet(globalBindingSet); // set 0
    state.addBindingSet(_bindlessTable);   // set 1: bindless textures
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    state.addVertexBuffer(nvrhi::VertexBufferBinding{in.vertexBuffer, 0, 0});
    state.indexBuffer    = nvrhi::IndexBufferBinding{in.indexBuffer, nvrhi::Format::R32_UINT, 0};
    state.indirectParams = in.indirectBuffer;
    commandList->setGraphicsState(state);

    commandList->drawIndexedIndirect(/*offsetBytes=*/ 0, in.commandCount);

    // Survivor/batch tallies come from the culler's readback; report the one call.
    stats.drawCalls = 1;
    return stats;
}

} // namespace Assisi::Render
