/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/PictureMips.hpp>

#include <algorithm>
#include <bit>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{

uint32_t PictureMipCount(uint32_t width, uint32_t height)
{
    return static_cast<uint32_t>(std::bit_width(std::max({width, height, 1u})));
}

bool PictureMips::Initialize(nvrhi::IDevice *device, nvrhi::Format format, const std::string &vertexSpvPath,
                             const std::string &pixelSpvPath)
{
    _device = device;
    _vertexShader = LoadSpirvShader(_device, vertexSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader = LoadSpirvShader(_device, pixelSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        Core::Log::Error("PictureMips: failed to load its shaders.");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _bindingLayout = _device->createBindingLayout(layoutDesc);

    // Bilinear, so one tap between four pixels averages them.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sampler = _device->createSampler(samplerDesc);

    nvrhi::FramebufferInfo levelInfo;
    levelInfo.colorFormats.push_back(format);
    levelInfo.sampleCount = 1;

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = _vertexShader;
    pipelineDesc.PS = _pixelShader;
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
    _pipeline = _bindingLayout && _sampler ? _device->createGraphicsPipeline(pipelineDesc, levelInfo) : nullptr;
    if (_pipeline == nullptr)
    {
        Core::Log::Error("PictureMips: failed to create its pipeline.");
        return false;
    }
    return true;
}

PictureMips::Chain PictureMips::Prepare(nvrhi::ITexture *picture) const
{
    Chain chain;
    if (!IsValid() || picture == nullptr)
    {
        return chain;
    }
    const nvrhi::TextureDesc &desc = picture->getDesc();
    for (uint32_t level = 1; level < desc.mipLevels; ++level)
    {
        nvrhi::FramebufferDesc framebuffer;
        framebuffer.addColorAttachment(picture, nvrhi::TextureSubresourceSet(level, 1, 0, 1));
        nvrhi::BindingSetDesc bindings;
        bindings.addItem(nvrhi::BindingSetItem::Texture_SRV(0, picture, nvrhi::Format::UNKNOWN,
                                                            nvrhi::TextureSubresourceSet(level - 1, 1, 0, 1)));
        bindings.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
        Chain::Level made{.framebuffer = _device->createFramebuffer(framebuffer),
                          .bindings = _device->createBindingSet(bindings, _bindingLayout),
                          .width = std::max(desc.width >> level, 1u),
                          .height = std::max(desc.height >> level, 1u)};
        if (made.framebuffer == nullptr || made.bindings == nullptr)
        {
            Core::Log::Error("PictureMips: the draws into a {}x{} picture's levels could not be made; it shows "
                             "without them.",
                             desc.width, desc.height);
            return Chain{};
        }
        chain.levels.push_back(std::move(made));
    }
    return chain;
}

void PictureMips::Build(nvrhi::ICommandList *commandList, const Chain &chain) const
{
    for (const Chain::Level &level : chain.levels)
    {
        nvrhi::GraphicsState state;
        state.pipeline = _pipeline;
        state.framebuffer = level.framebuffer;
        state.addBindingSet(level.bindings);
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(level.width), static_cast<float>(level.height)));
        commandList->setGraphicsState(state);
        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount = 3;
        commandList->draw(drawArgs);
    }
}

} // namespace Assisi::Render
