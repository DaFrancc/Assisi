/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshPass.hpp>

#include <Assisi/Render/DefaultResources.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <cstddef>
#include <iterator>

namespace Assisi::Render
{

bool MeshPass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo,
                          const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath)
{
    _device = device;

    const nvrhi::ShaderHandle vertexShader = LoadSpirvShader(device, vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    const nvrhi::ShaderHandle fragmentShader = LoadSpirvShader(device, pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!vertexShader || !fragmentShader)
    {
        return false;
    }

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
    _inputLayout = device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), vertexShader);

    // Texture_SRV/Sampler are separate descriptors in NVRHI's Vulkan backend (HLSL
    // t-register/s-register split, not GLSL's combined sampler2D) — see
    // cube_min.frag for the matching `texture2D` + `sampler` declarations.
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::mat4)));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _bindingLayout = device->createBindingLayout(bindingLayoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Repeat);
    _sampler = device->createSampler(samplerDesc);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.inputLayout = _inputLayout;
    pipelineDesc.VS = vertexShader;
    pipelineDesc.PS = fragmentShader;
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    // Meshes are authored CCW-front (standard convention). NVRHI's Vulkan backend
    // flips the viewport (VKViewportWithDXCoords) to undo Vulkan's native Y-down
    // clip space, which also flips the winding order the rasterizer perceives —
    // without this, back-face culling culls the actual front faces instead.
    pipelineDesc.renderState.rasterState.frontCounterClockwise = true;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;

    _pipeline = device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    return _pipeline != nullptr;
}

nvrhi::IBindingSet *MeshPass::GetOrCreateBindingSet(nvrhi::ITexture *albedoTexture) const
{
    const auto it = _bindingSetCache.find(albedoTexture);
    if (it != _bindingSetCache.end())
    {
        return it->second;
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::mat4)));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, albedoTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
    const nvrhi::BindingSetHandle bindingSet = _device->createBindingSet(bindingSetDesc, _bindingLayout);

    return _bindingSetCache.emplace(albedoTexture, bindingSet).first->second;
}

void MeshPass::Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
                     uint32_t viewportHeight, const glm::mat4 &modelViewProjection, const MeshBuffer &mesh,
                     nvrhi::ITexture *albedoTexture) const
{
    nvrhi::ITexture *resolvedAlbedo = albedoTexture != nullptr ? albedoTexture : DefaultResources::WhiteTexture(_device);

    nvrhi::GraphicsState state;
    state.pipeline = _pipeline;
    state.framebuffer = framebuffer;
    state.addBindingSet(GetOrCreateBindingSet(resolvedAlbedo));
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
    state.addVertexBuffer(nvrhi::VertexBufferBinding{mesh.VertexBuffer(), 0, 0});
    state.indexBuffer = nvrhi::IndexBufferBinding{mesh.IndexBuffer(), nvrhi::Format::R32_UINT, 0};
    commandList->setGraphicsState(state);

    commandList->setPushConstants(&modelViewProjection, sizeof(modelViewProjection));

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = mesh.IndexCount();
    commandList->drawIndexed(drawArgs);
}

} // namespace Assisi::Render
