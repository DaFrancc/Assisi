/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/BloomPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{
namespace
{

// Each copy's name in a capture.
constexpr std::array<const char *, kMaxBloomLevels> kLevelNames{
    "Bloom::Level0", "Bloom::Level1", "Bloom::Level2", "Bloom::Level3", "Bloom::Level4",
    "Bloom::Level5", "Bloom::Level6", "Bloom::Level7", "Bloom::Level8", "Bloom::Level9"};

} // namespace

bool BloomPass::Initialize(nvrhi::IDevice *device, nvrhi::IShader *fullscreenVertexShader,
                           const std::string &downShaderSpvPath, const std::string &upShaderSpvPath)
{
    _device = device;
    _downShader = LoadSpirvShader(_device, downShaderSpvPath, nvrhi::ShaderType::Pixel);
    _upShader = LoadSpirvShader(_device, upShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_downShader || !_upShader || fullscreenVertexShader == nullptr)
    {
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BloomConstants)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _bindingLayout = _device->createBindingLayout(layoutDesc);

    // Bilinear, so each of a downsample's taps averages four pixels.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sampler = _device->createSampler(samplerDesc);

    nvrhi::FramebufferInfo levelInfo;
    levelInfo.colorFormats.push_back(kBloomFormat);
    levelInfo.sampleCount = 1;

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = fullscreenVertexShader;
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;

    pipelineDesc.PS = _downShader;
    _downPipeline = _device->createGraphicsPipeline(pipelineDesc, levelInfo);

    // Climbing up blends the smaller copy over the larger by the scatter, which
    // the shader writes as its alpha: what the larger copy held is its own
    // downsample, and it keeps one less the scatter of it.
    nvrhi::BlendState::RenderTarget &blend = pipelineDesc.renderState.blendState.targets[0];
    blend.blendEnable = true;
    blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    blend.srcBlendAlpha = nvrhi::BlendFactor::Zero;
    blend.destBlendAlpha = nvrhi::BlendFactor::One;
    pipelineDesc.PS = _upShader;
    _upPipeline = _device->createGraphicsPipeline(pipelineDesc, levelInfo);

    return _bindingLayout && _sampler && _downPipeline && _upPipeline;
}

void BloomPass::Configure(uint32_t width, uint32_t height, nvrhi::ITexture *source)
{
    for (Level &level : _levels)
    {
        level = Level{};
    }
    _levelCount = 0u;
    _sourceExtent = glm::uvec2(width, height);
    if (_device == nullptr || source == nullptr || width == 0u || height == 0u)
    {
        return;
    }

    const uint32_t count = BloomLevelCount(height);
    for (uint32_t i = 0; i < count; ++i)
    {
        Level &level = _levels[i];
        level.extent = BloomLevelExtent(width, height, i);

        nvrhi::TextureDesc desc;
        desc.width = level.extent.x;
        desc.height = level.extent.y;
        desc.format = kBloomFormat;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.debugName = kLevelNames[i];
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        level.texture = _device->createTexture(desc);
        if (!level.texture)
        {
            Core::Log::Error("Bloom: could not create a {}x{} copy; bloom is off.", level.extent.x, level.extent.y);
            Configure(0u, 0u, nullptr);
            return;
        }

        nvrhi::FramebufferDesc framebufferDesc;
        framebufferDesc.addColorAttachment(level.texture);
        level.framebuffer = _device->createFramebuffer(framebufferDesc);
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        Level &level = _levels[i];
        nvrhi::ITexture *above = i == 0 ? source : _levels[i - 1].texture.Get();
        nvrhi::BindingSetDesc down;
        down.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)));
        down.addItem(nvrhi::BindingSetItem::Texture_SRV(0, above));
        down.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
        level.downBindings = _device->createBindingSet(down, _bindingLayout);

        if (i + 1 < count)
        {
            nvrhi::BindingSetDesc up;
            up.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomConstants)));
            up.addItem(nvrhi::BindingSetItem::Texture_SRV(0, _levels[i + 1].texture));
            up.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
            level.upBindings = _device->createBindingSet(up, _bindingLayout);
        }
    }
    _levelCount = count;
}

nvrhi::ITexture *BloomPass::Output() const
{
    return Ready() ? _levels[0].texture.Get() : nullptr;
}

void BloomPass::Draw(nvrhi::ICommandList *commandList, nvrhi::IGraphicsPipeline *pipeline, const Level &target,
                     nvrhi::IBindingSet *bindings, const BloomConstants &constants) const
{
    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = target.framebuffer;
    state.addBindingSet(bindings);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(target.extent.x), static_cast<float>(target.extent.y)));
    commandList->setGraphicsState(state);
    commandList->setPushConstants(&constants, sizeof(constants));
    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = 3;
    commandList->draw(drawArgs);
}

void BloomPass::Run(nvrhi::ICommandList *commandList, float scatter) const
{
    if (!Ready())
    {
        return;
    }

    for (uint32_t i = 0; i < _levelCount; ++i)
    {
        const glm::uvec2 above = i == 0 ? _sourceExtent : _levels[i - 1].extent;
        const BloomConstants constants{.texelSize = 1.0f / glm::vec2(above), .karis = i == 0 ? 1u : 0u};
        Draw(commandList, _downPipeline, _levels[i], _levels[i].downBindings, constants);
    }

    for (uint32_t i = _levelCount - 1u; i-- > 0u;)
    {
        const BloomConstants constants{.texelSize = 1.0f / glm::vec2(_levels[i + 1].extent), .scatter = scatter};
        Draw(commandList, _upPipeline, _levels[i], _levels[i].upBindings, constants);
    }
}

void BloomPass::Shutdown()
{
    Configure(0u, 0u, nullptr);
    _upPipeline = nullptr;
    _downPipeline = nullptr;
    _bindingLayout = nullptr;
    _sampler = nullptr;
    _upShader = nullptr;
    _downShader = nullptr;
    _device = nullptr;
}

} // namespace Assisi::Render
