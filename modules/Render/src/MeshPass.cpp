/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshPass.hpp>

#include <Assisi/Core/Logger.hpp>
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
// draw sets startInstanceLocation to its record). Mirrors cube_min.vert's
// `InstanceData` std430 struct — mat4 (0..63) + uint (64), padded to the 16-byte
// array stride std430 gives the struct.
struct InstanceData
{
    glm::mat4 model;
    uint32_t  materialIndex; // row into the material table (== Material::Id()).
    uint32_t  _pad0 = 0, _pad1 = 0, _pad2 = 0;
};
static_assert(sizeof(InstanceData) == 80, "InstanceData must match the shader's std430 array stride.");

// Starting instance-buffer capacity (records). Grows geometrically past this when
// a frame draws more items; the first level typically fits without a growth.
constexpr uint32_t kInitialInstanceCapacity = 1024u;

// Per-frame data (view-projection + camera + cluster-grid parameters), uploaded
// once per frame via UpdateFrameConstants() into a constant buffer. viewProjection
// leads so the vertex shader can form clip position from each instance's world
// matrix. Mirrors cube_min.vert/frag's matching `uniform FrameConstants` block.
struct FrameConstants
{
    glm::mat4  viewProjection;
    glm::mat4  view;
    glm::uvec4 gridDim;           // xyz used, w unused
    glm::vec4  screenSizeNearFar; // xy = screen size, z = nearZ, w = farZ
    glm::uvec4 lightCounts;       // x = directional light count, y = debug view, zw unused
};
} // namespace

bool MeshPass::Initialize(const InitParams &params)
{
    nvrhi::IDevice *const           device = params.device;
    const nvrhi::FramebufferInfo   &framebufferInfo = params.framebufferInfo;
    const std::string              &vertexShaderSpvPath = params.vertexShaderSpvPath;
    const std::string              &pixelShaderSpvPath = params.pixelShaderSpvPath;

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
    // with Texture_SRV. Slot map (see cube_min.vert/frag's matching `binding = N`):
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
                                    float farZ, uint32_t dirLightCount, MaterialDebugView debugView) const
{
    FrameConstants constants;
    constants.viewProjection = viewProjection;
    constants.view = view;
    constants.gridDim = glm::uvec4(ClusterGrid::kNumX, ClusterGrid::kNumY, ClusterGrid::kNumZ, 0u);
    constants.screenSizeNearFar =
        glm::vec4(static_cast<float>(screenWidth), static_cast<float>(screenHeight), nearZ, farZ);
    constants.lightCounts = glm::uvec4(dirLightCount, static_cast<uint32_t>(debugView), 0u, 0u);
    commandList->writeBuffer(_frameConstantsBuffer, &constants, sizeof(constants));
}

nvrhi::IBindingSet *MeshPass::GetOrCreateGlobalBindingSet() const
{
    nvrhi::IBuffer *const instanceBuffer = _instanceBuffer.NativeBuffer();
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

MeshPass::SubmitStats MeshPass::Submit(const RenderFrame &frame, std::span<const DrawItem> items) const
{
    nvrhi::ICommandList *const commandList = frame.commandList;
    SubmitStats                stats;

    // Gather per-object data for every valid item, in submit order. The draw loop
    // below skips the same nulls in the same order, so instance record i lines up
    // with the i-th draw's startInstanceLocation (its gl_InstanceIndex).
    std::vector<InstanceData> instances;
    instances.reserve(items.size());
    for (const DrawItem &item : items)
    {
        if (item.material == nullptr || item.mesh == nullptr)
        {
            continue; // producer should have filtered these; belt and suspenders
        }
        instances.push_back(InstanceData{item.model, item.material->Id()});
    }
    if (instances.empty())
    {
        return stats; // nothing to draw — leave the instance buffer/set untouched
    }

    // Grow the instance buffer to hold this frame's records if needed. A growth
    // swaps the buffer handle, so the cached global set (which references it) must
    // be rebuilt — GetOrCreateGlobalBindingSet notices via _globalSetInstanceBuffer.
    if (!_instanceBuffer.IsValid() || instances.size() > _instanceBuffer.CapacityElements())
    {
        const uint32_t needed = static_cast<uint32_t>(instances.size());
        const uint32_t grown = std::max(_instanceBuffer.CapacityElements() * 2u, kInitialInstanceCapacity);
        _instanceBuffer.Create(_device, sizeof(InstanceData), std::max(grown, needed), /*allowUnorderedAccess=*/false,
                               "MeshPass::Instances");
    }
    // Upload before recording draws: the copy lands in this command list ahead of
    // the draws that read it, so the ordering is correct within the frame.
    _instanceBuffer.Upload(commandList, instances.data(), static_cast<uint32_t>(instances.size()));

    nvrhi::IBindingSet *const globalBindingSet = GetOrCreateGlobalBindingSet();

    // Distinct material-id / vertex-buffer runs the sort produced — batching
    // diagnostics now that binds are frame-global (see SubmitStats). UINT32_MAX /
    // nullptr never match a real id/pointer, so the first item counts as both.
    uint32_t              prevMaterialId   = UINT32_MAX;
    const nvrhi::IBuffer *prevVertexBuffer = nullptr;

    uint32_t instanceIndex = 0;
    for (const DrawItem &item : items)
    {
        if (item.material == nullptr || item.mesh == nullptr)
        {
            continue; // same filter as the gather loop, so instanceIndex stays aligned
        }

        if (item.material->Id() != prevMaterialId)
        {
            ++stats.materialBinds;
            prevMaterialId = item.material->Id();
        }
        if (item.mesh->VertexBuffer() != prevVertexBuffer)
        {
            ++stats.meshBinds;
            prevVertexBuffer = item.mesh->VertexBuffer();
        }

        const Geometry::SubMesh &subMesh = item.mesh->SubMeshes()[item.submeshIndex];

        // GraphicsState is a cheap value; rebuild it each item rather than mutate
        // its static_vectors. NVRHI compares it against the cached state and only
        // re-binds what actually changed since the previous item — with one global
        // set that's just the vertex/index buffers (already identical via the arena).
        nvrhi::GraphicsState state;
        state.pipeline = _pipeline;
        state.framebuffer = frame.framebuffer;
        state.addBindingSet(globalBindingSet); // set 0: frame/sampler/lights/material table/instances
        state.addBindingSet(_bindlessTable);   // set 1: bindless textures
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
        state.addVertexBuffer(nvrhi::VertexBufferBinding{item.mesh->VertexBuffer(), 0, 0});
        state.indexBuffer = nvrhi::IndexBufferBinding{item.mesh->IndexBuffer(), nvrhi::Format::R32_UINT, 0};
        commandList->setGraphicsState(state);

        // Arena addressing: index values stay mesh-local, so baseVertex
        // (startVertexLocation) shifts them into this mesh's vertex range, and
        // startIndexLocation picks the mesh's slice of the shared index buffer.
        // startInstanceLocation makes gl_InstanceIndex select this item's record.
        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = subMesh.IndexCount; // for drawIndexed this is the index count
        drawArgs.startIndexLocation = item.mesh->IndexBase() + subMesh.IndexOffset;
        drawArgs.startVertexLocation = item.mesh->VertexBase();
        drawArgs.startInstanceLocation = instanceIndex;
        commandList->drawIndexed(drawArgs);
        ++stats.drawCalls;
        ++instanceIndex;
    }

    return stats;
}

} // namespace Assisi::Render
