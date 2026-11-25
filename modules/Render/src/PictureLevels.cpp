/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/PictureLevels.hpp>

#include <algorithm>
#include <bit>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{
namespace
{

/// Half a pixel: the last coordinate a bilinear tap can take inside a region
/// without blending in the pixel past its edge is half a pixel short of it.
constexpr float kHalfPixel = 0.5f;

/// The narrowest fade a vignette draws with, in picture heights: a hard edge
/// to the eye, and the shader never divides by zero.
constexpr float kLeastVignetteSoftness = 1e-4f;

} // namespace

uint32_t PictureMipCount(uint32_t width, uint32_t height)
{
    return static_cast<uint32_t>(std::bit_width(std::max({width, height, 1u})));
}

bool PictureLevels::Initialize(nvrhi::IDevice *device, nvrhi::Format format, const std::string &vertexSpvPath,
                               const std::string &pixelSpvPath)
{
    _device = device;
    _vertexShader = LoadSpirvShader(_device, vertexSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader = LoadSpirvShader(_device, pixelSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        Core::Log::Error("PictureLevels: failed to load its shaders.");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _bindingLayout = _device->createBindingLayout(layoutDesc);

    // Bilinear: one tap between four pixels averages them, and a stretch
    // blends between the pixels it falls among.
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
        Core::Log::Error("PictureLevels: failed to create its pipeline.");
        return false;
    }
    return true;
}

PictureLevels::Chain PictureLevels::Prepare(nvrhi::ITexture *picture, nvrhi::ITexture *drawn) const
{
    Chain chain;
    if (!IsValid() || picture == nullptr || drawn == nullptr)
    {
        return chain;
    }
    const nvrhi::TextureDesc &desc = picture->getDesc();
    for (uint32_t level = 0; level < desc.mipLevels; ++level)
    {
        nvrhi::FramebufferDesc framebuffer;
        framebuffer.addColorAttachment(picture, nvrhi::TextureSubresourceSet(level, 1, 0, 1));
        nvrhi::BindingSetDesc bindings;
        bindings.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)));
        bindings.addItem(level == 0 ? nvrhi::BindingSetItem::Texture_SRV(0, drawn)
                                    : nvrhi::BindingSetItem::Texture_SRV(0, picture, nvrhi::Format::UNKNOWN,
                                                                         nvrhi::TextureSubresourceSet(level - 1, 1, 0,
                                                                                                      1)));
        bindings.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
        Level made{.framebuffer = _device->createFramebuffer(framebuffer),
                   .bindings = _device->createBindingSet(bindings, _bindingLayout),
                   .width = std::max(desc.width >> level, 1u),
                   .height = std::max(desc.height >> level, 1u)};
        if (made.framebuffer == nullptr || made.bindings == nullptr)
        {
            Core::Log::Error("PictureLevels: the draws into a {}x{} picture could not be made; it shows nothing.",
                             desc.width, desc.height);
            return Chain{};
        }
        chain.levels.push_back(std::move(made));
    }
    return chain;
}

void PictureLevels::Build(nvrhi::ICommandList *commandList, const Chain &chain, uint32_t drawnWidth,
                          uint32_t drawnHeight, const Vignette &vignette) const
{
    if (chain.levels.empty())
    {
        return;
    }
    // The first level reads the drawn part of a texture its own size, and
    // stops half a pixel inside that part's far edges. It alone is darkened.
    const Level &first = chain.levels.front();
    const float width = static_cast<float>(first.width);
    const float height = static_cast<float>(first.height);
    const float drawnX = static_cast<float>(std::clamp(drawnWidth, 1u, first.width));
    const float drawnY = static_cast<float>(std::clamp(drawnHeight, 1u, first.height));
    Draw(commandList, first,
         Constants{.scaleX = drawnX / width,
                   .scaleY = drawnY / height,
                   .mostX = (drawnX - kHalfPixel) / width,
                   .mostY = (drawnY - kHalfPixel) / height,
                   .centreX = vignette.centre.x,
                   .centreY = vignette.centre.y,
                   .radius = std::max(vignette.radius, 0.f),
                   .softness = std::max(vignette.softness, kLeastVignetteSoftness),
                   .strength = std::clamp(vignette.strength, 0.f, 1.f),
                   .aspect = width / height});
    for (std::size_t level = 1; level < chain.levels.size(); ++level)
    {
        Draw(commandList, chain.levels[level], Constants{});
    }
}

void PictureLevels::Draw(nvrhi::ICommandList *commandList, const Level &level, const Constants &constants) const
{
    nvrhi::GraphicsState state;
    state.pipeline = _pipeline;
    state.framebuffer = level.framebuffer;
    state.addBindingSet(level.bindings);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(level.width), static_cast<float>(level.height)));
    commandList->setGraphicsState(state);
    commandList->setPushConstants(&constants, sizeof(constants));
    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = 3;
    commandList->draw(drawArgs);
}

} // namespace Assisi::Render
