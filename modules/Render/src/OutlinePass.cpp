/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/OutlinePass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace Assisi::Render
{

namespace
{
// The coverage mask only needs one channel (in/out of the silhouette), so R8 is
// a quarter the memory/bandwidth of a colour target.
constexpr nvrhi::Format kMaskFormat = nvrhi::Format::R8_UNORM;

// Outline thickness in screen pixels. The edge pass paints this far outside the
// silhouette; bump it for a chunkier border.
constexpr float kOutlineWidthPx = 2.0f;
} // namespace

bool OutlinePass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &sceneFramebufferInfo,
                             uint32_t width, uint32_t height, const std::string &maskVertexShaderSpvPath,
                             const std::string &maskPixelShaderSpvPath,
                             const std::string &edgeVertexShaderSpvPath,
                             const std::string &edgePixelShaderSpvPath)
{
    _device = device;

    _maskVertexShader = LoadSpirvShader(device, maskVertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _maskPixelShader  = LoadSpirvShader(device, maskPixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    _edgeVertexShader = LoadSpirvShader(device, edgeVertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _edgePixelShader  = LoadSpirvShader(device, edgePixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_maskVertexShader || !_maskPixelShader || !_edgeVertexShader || !_edgePixelShader)
    {
        return false;
    }

    // Mask pass reads position only — coverage doesn't care about normals/UVs.
    using Assisi::Geometry::Vertex;
    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(offsetof(Vertex, Position))
            .setElementStride(sizeof(Vertex)),
    };
    _maskInputLayout =
        device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), _maskVertexShader);

    // Mask pass: just the per-draw MVP push constant.
    nvrhi::BindingLayoutDesc maskLayoutDesc;
    maskLayoutDesc.visibility = nvrhi::ShaderType::Vertex;
    maskLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::mat4)));
    _maskBindingLayout = device->createBindingLayout(maskLayoutDesc);

    nvrhi::BindingSetDesc maskSetDesc;
    maskSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::mat4)));
    _maskBindingSet = device->createBindingSet(maskSetDesc, _maskBindingLayout);

    // Edge pass: push constant (texel size + width) + the mask texture + a sampler.
    // Point sampling with clamp keeps the 0/1 mask crisp and stops the kernel from
    // reading past the edges.
    nvrhi::BindingLayoutDesc edgeLayoutDesc;
    edgeLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
    edgeLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::vec4)));
    edgeLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    edgeLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _edgeBindingLayout = device->createBindingLayout(edgeLayoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(false); // point — the mask is a hard 0/1 coverage
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sampler = device->createSampler(samplerDesc);

    if (!EnsureMask(width, height))
    {
        Core::Log::Error("OutlinePass: failed to create the coverage mask target.");
        return false;
    }

    // Mask pipeline: no cull (silhouette is the union of all faces), depth off (so
    // the whole silhouette is covered, even occluded), no blend — flat coverage.
    // Built against the mask target's own format; unaffected by scene MSAA changes.
    nvrhi::GraphicsPipelineDesc maskPipelineDesc;
    maskPipelineDesc.primType    = nvrhi::PrimitiveType::TriangleList;
    maskPipelineDesc.inputLayout = _maskInputLayout;
    maskPipelineDesc.VS          = _maskVertexShader;
    maskPipelineDesc.PS          = _maskPixelShader;
    maskPipelineDesc.addBindingLayout(_maskBindingLayout);
    maskPipelineDesc.renderState.rasterState.cullMode               = nvrhi::RasterCullMode::None;
    maskPipelineDesc.renderState.rasterState.frontCounterClockwise  = true;
    maskPipelineDesc.renderState.depthStencilState.depthTestEnable  = false;
    maskPipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
    _maskPipeline = device->createGraphicsPipeline(maskPipelineDesc, _maskFramebuffer->getFramebufferInfo());
    if (_maskPipeline == nullptr)
    {
        Core::Log::Error("OutlinePass: failed to create the mask pipeline.");
        return false;
    }

    return BuildEdgePipeline(sceneFramebufferInfo);
}

bool OutlinePass::BuildEdgePipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo)
{
    // Fullscreen edge detect (no vertex buffer — fullscreen.vert uses gl_VertexIndex),
    // depth off, alpha-blended so the orange border composites over the scene and
    // non-edge pixels leave it untouched (they output alpha 0).
    nvrhi::GraphicsPipelineDesc edgePipelineDesc;
    edgePipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    edgePipelineDesc.VS       = _edgeVertexShader;
    edgePipelineDesc.PS       = _edgePixelShader;
    edgePipelineDesc.addBindingLayout(_edgeBindingLayout);
    edgePipelineDesc.renderState.rasterState.cullMode               = nvrhi::RasterCullMode::None;
    edgePipelineDesc.renderState.depthStencilState.depthTestEnable  = false;
    edgePipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    nvrhi::BlendState::RenderTarget &blend = edgePipelineDesc.renderState.blendState.targets[0];
    blend.blendEnable    = true;
    blend.srcBlend       = nvrhi::BlendFactor::SrcAlpha;
    blend.destBlend      = nvrhi::BlendFactor::InvSrcAlpha;
    blend.blendOp        = nvrhi::BlendOp::Add;
    blend.srcBlendAlpha  = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
    blend.blendOpAlpha   = nvrhi::BlendOp::Add;

    _edgePipeline = _device->createGraphicsPipeline(edgePipelineDesc, sceneFramebufferInfo);
    if (_edgePipeline == nullptr)
    {
        Core::Log::Error("OutlinePass: failed to create the edge-detect pipeline.");
        return false;
    }
    return true;
}

bool OutlinePass::RebuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo)
{
    if (_edgeBindingLayout == nullptr)
    {
        return true; // nothing built yet — nothing to rebuild
    }
    return BuildEdgePipeline(sceneFramebufferInfo);
}

bool OutlinePass::EnsureMask(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return _maskTexture != nullptr; // minimised — keep whatever we have
    }
    if (width == _maskWidth && height == _maskHeight && _maskTexture != nullptr)
    {
        return true;
    }

    nvrhi::TextureDesc maskDesc;
    maskDesc.width            = width;
    maskDesc.height           = height;
    maskDesc.format           = kMaskFormat;
    maskDesc.isRenderTarget   = true; // the mask pass draws into it...
    maskDesc.isShaderResource = true; // ...and the edge pass samples it
    maskDesc.debugName        = "OutlinePass::CoverageMask";
    maskDesc.initialState     = nvrhi::ResourceStates::RenderTarget;
    maskDesc.keepInitialState = true;
    _maskTexture              = _device->createTexture(maskDesc);
    if (_maskTexture == nullptr)
    {
        return false;
    }

    nvrhi::FramebufferDesc framebufferDesc;
    framebufferDesc.addColorAttachment(_maskTexture);
    _maskFramebuffer = _device->createFramebuffer(framebufferDesc);
    if (_maskFramebuffer == nullptr)
    {
        return false;
    }

    // The edge pass's binding set names the (just recreated) mask texture.
    nvrhi::BindingSetDesc edgeSetDesc;
    edgeSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec4)));
    edgeSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, _maskTexture));
    edgeSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
    _edgeBindingSet = _device->createBindingSet(edgeSetDesc, _edgeBindingLayout);

    _maskWidth  = width;
    _maskHeight = height;
    return true;
}

void OutlinePass::RecordMaskPass(const RenderFrame &frame, const glm::mat4 &modelViewProjection,
                                 const MeshBuffer &mesh)
{
    nvrhi::ICommandList *const commandList = frame.commandList;

    // Fresh mask every frame — clear to 0 (not covered), then stamp the silhouette.
    commandList->clearTextureFloat(_maskTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));

    nvrhi::GraphicsState state;
    state.pipeline    = _maskPipeline;
    state.framebuffer = _maskFramebuffer;
    state.addBindingSet(_maskBindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(_maskWidth), static_cast<float>(_maskHeight)));
    state.addVertexBuffer(nvrhi::VertexBufferBinding{mesh.VertexBuffer(), 0, 0});
    state.indexBuffer = nvrhi::IndexBufferBinding{mesh.IndexBuffer(), nvrhi::Format::R32_UINT, 0};
    commandList->setGraphicsState(state);

    commandList->setPushConstants(&modelViewProjection, sizeof(modelViewProjection));

    // LOD0's submeshes — or the whole index buffer if the mesh has no LOD table
    // (e.g. a factory primitive).
    if (!mesh.Lods().empty() && !mesh.SubMeshes().empty())
    {
        const Geometry::LodRange &lod0 = mesh.Lods().front();
        for (uint32_t i = 0; i < lod0.SubMeshCount; ++i)
        {
            const Geometry::SubMesh &subMesh = mesh.SubMeshes()[lod0.FirstSubMesh + i];
            nvrhi::DrawArguments      drawArgs;
            drawArgs.vertexCount        = subMesh.IndexCount;
            drawArgs.startIndexLocation = subMesh.IndexOffset;
            commandList->drawIndexed(drawArgs);
        }
    }
    else
    {
        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = mesh.IndexCount();
        commandList->drawIndexed(drawArgs);
    }
}

void OutlinePass::RecordEdgePass(const RenderFrame &frame)
{
    nvrhi::ICommandList *const commandList = frame.commandList;

    nvrhi::GraphicsState state;
    state.pipeline    = _edgePipeline;
    state.framebuffer = frame.framebuffer;
    state.addBindingSet(_edgeBindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    commandList->setGraphicsState(state);

    // Mask texel size (matches the mask's own dimensions) + outline width in pixels.
    const glm::vec4 params(1.0f / static_cast<float>(_maskWidth), 1.0f / static_cast<float>(_maskHeight),
                           kOutlineWidthPx, 0.f);
    commandList->setPushConstants(&params, sizeof(params));

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = 3; // fullscreen triangle
    commandList->draw(drawArgs);
}

void OutlinePass::Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const MeshBuffer &mesh,
                       const glm::mat4 &model)
{
    if (!IsValid() || mesh.VertexBuffer() == nullptr || mesh.IndexBuffer() == nullptr)
    {
        return;
    }
    if (!EnsureMask(frame.width, frame.height))
    {
        return;
    }

    RecordMaskPass(frame, viewProjection * model, mesh);
    RecordEdgePass(frame);
}

} // namespace Assisi::Render
