/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/IconPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{

namespace
{
// Per-billboard vertex-stage constants (mirrors icon_billboard.vert). 112 bytes,
// within the 128-byte push-constant floor.
struct IconPushConstants
{
    glm::mat4 viewProjection;
    glm::vec4 center;    // xyz = entity world position
    glm::vec4 rightHalf; // xyz = camera right * half world size
    glm::vec4 upHalf;    // xyz = camera up    * half world size
};
} // namespace

bool IconPass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &sceneFramebufferInfo,
                          const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath,
                          const std::string &iconAssetPath)
{
    _device = device;

    _vertexShader = LoadSpirvShader(device, vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = LoadSpirvShader(device, pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        return false;
    }

    // Load the supplied icon. If the art isn't present yet, fall back to a solid
    // magenta placeholder so the feature is visibly wired up rather than absent.
    if (!_icon.LoadFromAssets(device, iconAssetPath, ColorSpace::Srgb).has_value())
    {
        Core::Log::Warn("IconPass: could not load icon '{}'; using a magenta placeholder.", iconAssetPath);
        _icon.UploadSolidColor(device, 255, 0, 255, 255, ColorSpace::Srgb, "IconPass::Placeholder");
    }
    if (!_icon.IsValid())
    {
        Core::Log::Error("IconPass: no icon texture (load and placeholder both failed).");
        return false;
    }

    // Push constants (billboard transform, read by the VS) + the icon texture and
    // its sampler (read by the PS). One layout spanning both stages.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(IconPushConstants)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _bindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true); // trilinear — the icon has a mip chain
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sampler = device->createSampler(samplerDesc);

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(IconPushConstants)));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, _icon.NativeTexture()));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
    _bindingSet = device->createBindingSet(setDesc, _bindingLayout);

    return BuildPipeline(sceneFramebufferInfo);
}

bool IconPass::BuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo)
{
    // No vertex buffer (corners come from gl_VertexIndex). No cull — a billboard's
    // winding flips as it faces the camera. Depth-tested so geometry occludes it,
    // but no depth write so overlapping icons blend. Alpha-blended composite.
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS       = _vertexShader;
    pipelineDesc.PS       = _pixelShader;
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode                  = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.depthStencilState.depthTestEnable     = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable    = false;
    pipelineDesc.renderState.depthStencilState.depthFunc           = nvrhi::ComparisonFunc::LessOrEqual;

    nvrhi::BlendState::RenderTarget &blend = pipelineDesc.renderState.blendState.targets[0];
    blend.blendEnable    = true;
    blend.srcBlend       = nvrhi::BlendFactor::SrcAlpha;
    blend.destBlend      = nvrhi::BlendFactor::InvSrcAlpha;
    blend.blendOp        = nvrhi::BlendOp::Add;
    blend.srcBlendAlpha  = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
    blend.blendOpAlpha   = nvrhi::BlendOp::Add;

    _pipeline = _device->createGraphicsPipeline(pipelineDesc, sceneFramebufferInfo);
    if (_pipeline == nullptr)
    {
        Core::Log::Error("IconPass: failed to create the billboard pipeline.");
        return false;
    }
    return true;
}

bool IconPass::RebuildPipeline(const nvrhi::FramebufferInfo &sceneFramebufferInfo)
{
    if (_bindingLayout == nullptr)
    {
        return true; // nothing built yet — nothing to rebuild
    }
    return BuildPipeline(sceneFramebufferInfo);
}

void IconPass::Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &cameraRight,
                    const glm::vec3 &cameraUp, std::span<const glm::vec3> positions)
{
    if (!IsValid() || positions.empty())
    {
        return;
    }

    nvrhi::GraphicsState state;
    state.pipeline    = _pipeline;
    state.framebuffer = frame.framebuffer;
    state.addBindingSet(_bindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    frame.commandList->setGraphicsState(state);

    // The camera basis and transform are the same for every icon; only the centre
    // changes, so set the state once and vary the push constant per billboard.
    constexpr float   half = 0.5f * kEntityIconWorldSize;
    IconPushConstants pc;
    pc.viewProjection = viewProjection;
    pc.rightHalf      = glm::vec4(cameraRight * half, 0.f);
    pc.upHalf         = glm::vec4(cameraUp * half, 0.f);

    for (const glm::vec3 &position : positions)
    {
        pc.center = glm::vec4(position, 1.f);
        frame.commandList->setPushConstants(&pc, sizeof(pc));

        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = 6; // two triangles
        frame.commandList->draw(drawArgs);
    }
}

} // namespace Assisi::Render
