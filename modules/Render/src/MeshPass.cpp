/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <cstddef>
#include <iterator>

namespace Assisi::Render
{

namespace
{
// Per-draw data, pushed as push constants (128 bytes — the portable Vulkan
// minimum, see cube_min.vert's matching push_constant block).
struct DrawPushConstants
{
    glm::mat4 modelViewProjection;
    glm::mat4 model;
};
static_assert(sizeof(DrawPushConstants) == 128);

// Per-frame data (camera + cluster-grid parameters), uploaded once per frame
// via UpdateFrameConstants() into a constant buffer rather than push
// constants — it doesn't vary per draw, and there wasn't room left in the
// push-constant budget alongside the two matrices above. Mirrors
// cube_min.vert/frag's matching `uniform FrameConstants` block.
struct FrameConstants
{
    glm::mat4 view;
    glm::uvec4 gridDim;           // xyz used, w unused
    glm::vec4 screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    glm::uvec4 lightCounts;       // x = directional light count, yzw unused
};
} // namespace

bool MeshPass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo,
                          const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath,
                          const ClusterGrid &clusterGrid)
{
    _device = device;
    _clusterGrid = &clusterGrid;

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

    // Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend (HLSL
    // t-register/s-register split, not GLSL's combined sampler2D); StructuredBuffer_SRV
    // shares the same t-register space as Texture_SRV. Slot map (see cube_min.frag
    // for the matching `binding = N` declarations):
    //   b0 = FrameConstants, b1 = MaterialConstants
    //   t0 = baseColor, t1-t5 = clustered light buffers, t6-t9 = normal/MR/occlusion/emissive
    //   s0 = shared sampler
    // The material SRVs sit past the light buffers rather than contiguous with t0
    // on purpose — the whole set collapses into one bindless table later, so the
    // gap is harmless; don't "tidy" them adjacent.
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawPushConstants)));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)); // FrameConstants
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1)); // MaterialConstants
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));    // baseColor
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1)); // pointLights
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2)); // spotLights
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3)); // dirLights
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4)); // lightIndexList
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5)); // lightGrid
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(6));          // normal
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(7));          // metallicRoughness
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(8));          // occlusion
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(9));          // emissive
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
    pipelineDesc.addBindingLayout(_bindingLayout);
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

void MeshPass::UpdateFrameConstants(nvrhi::ICommandList *commandList, const glm::mat4 &view, uint32_t screenWidth,
                                    uint32_t screenHeight, float nearZ, float farZ, uint32_t dirLightCount) const
{
    FrameConstants constants;
    constants.view = view;
    constants.gridDim = glm::uvec4(ClusterGrid::kNumX, ClusterGrid::kNumY, ClusterGrid::kNumZ, 0u);
    constants.screenSizeNearFar =
        glm::vec4(static_cast<float>(screenWidth), static_cast<float>(screenHeight), nearZ, farZ);
    constants.lightCounts = glm::uvec4(dirLightCount, 0u, 0u, 0u);
    commandList->writeBuffer(_frameConstantsBuffer, &constants, sizeof(constants));
}

nvrhi::IBindingSet *MeshPass::GetOrCreateBindingSet(const Material &material) const
{
    const auto it = _bindingSetCache.find(material.Id());
    if (it != _bindingSetCache.end())
    {
        return it->second;
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(DrawPushConstants)));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _frameConstantsBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(1, material.Constants()));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, material.BaseColor()));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
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
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(6, material.Normal()));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(7, material.MetallicRoughness()));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(8, material.Occlusion()));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(9, material.Emissive()));
    const nvrhi::BindingSetHandle bindingSet = _device->createBindingSet(bindingSetDesc, _bindingLayout);

    return _bindingSetCache.emplace(material.Id(), bindingSet).first->second;
}

void MeshPass::Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
                     uint32_t viewportHeight, const glm::mat4 &modelViewProjection, const glm::mat4 &model,
                     const MeshBuffer &mesh, std::span<const Material *const> materials) const
{
    const DrawPushConstants pushConstants{modelViewProjection, model};

    // LOD0 only for now (screen-size LOD selection lands with the draw-item layer).
    // EnsureSubMeshTables guarantees at least one LOD spanning one submesh.
    const std::vector<Geometry::SubMesh> &subMeshes = mesh.SubMeshes();
    const std::vector<Geometry::LodRange> &lods = mesh.Lods();
    const Geometry::LodRange lod0 =
        !lods.empty() ? lods.front() : Geometry::LodRange{0, static_cast<uint32_t>(subMeshes.size())};

    for (uint32_t i = 0; i < lod0.SubMeshCount; ++i)
    {
        const Geometry::SubMesh &subMesh = subMeshes[lod0.FirstSubMesh + i];
        const Material *material = subMesh.MaterialSlot < materials.size() ? materials[subMesh.MaterialSlot] : nullptr;
        if (material == nullptr)
            continue; // No material resolved for this slot — skip rather than guess.

        // Everything but the binding set is identical per submesh, but GraphicsState
        // is a cheap value; rebuild it each time rather than mutate its static_vectors.
        nvrhi::GraphicsState state;
        state.pipeline = _pipeline;
        state.framebuffer = framebuffer;
        state.addBindingSet(GetOrCreateBindingSet(*material));
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
        state.addVertexBuffer(nvrhi::VertexBufferBinding{mesh.VertexBuffer(), 0, 0});
        state.indexBuffer = nvrhi::IndexBufferBinding{mesh.IndexBuffer(), nvrhi::Format::R32_UINT, 0};
        commandList->setGraphicsState(state);
        commandList->setPushConstants(&pushConstants, sizeof(pushConstants));

        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = subMesh.IndexCount; // for drawIndexed this is the index count
        drawArgs.startIndexLocation = subMesh.IndexOffset;
        commandList->drawIndexed(drawArgs);
    }
}

} // namespace Assisi::Render
