/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Engine/QuadPass.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Image/Image.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Assisi::Mondrian::Engine
{
namespace
{

constexpr const char *kVertexShaderPath = "shaders/mondrian_quad.vert.spv";
constexpr const char *kPixelShaderPath  = "shaders/mondrian_quad.frag.spv";

/// Two triangles, generated in the vertex shader from the vertex index.
constexpr uint32_t kVerticesPerQuad = 6;

/// Instances the buffer is first made for. A menu is a few dozen quads, so the
/// first growth waits until a screen is genuinely busy.
constexpr uint32_t kInitialInstanceCapacity = 256;

/// The white texture's one texel, and the value it holds.
constexpr unsigned char kWhiteTexel = 255;

/// Mirrors the push-constant block in mondrian/quad.glsl member for member.
struct PassConstants
{
    std::array<float, 2> viewport; ///< target width and height in pixels
    uint32_t encodeSrgb;           ///< nonzero when the target encodes sRGB on write
};
// The shader reads this block at fixed offsets.
static_assert(sizeof(PassConstants) == 3 * sizeof(uint32_t));

/// The placeholder checkerboard: side in texels, the side of one square, and
/// its two colours as RGBA bytes.
constexpr uint32_t kCheckerSide       = 64;
constexpr uint32_t kCheckerSquareSide = 8;
constexpr std::array<unsigned char, 4> kCheckerLight{240, 240, 240, 255};
constexpr std::array<unsigned char, 4> kCheckerDark{40, 90, 200, 255};
constexpr uint32_t kRgba8Bytes = 4;

} // namespace

bool QuadPass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo)
{
    _device = device;

    _vertexShader = Render::LoadSpirvShader(device, kVertexShaderPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = Render::LoadSpirvShader(device, kPixelShaderPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        Core::Log::Error("Mondrian: the quad shaders could not be loaded.");
        return false;
    }

    // The target's own format decides, not an assumption about the swapchain:
    // an sRGB target would otherwise lighten every display colour the UI writes.
    _encodeSrgb = !framebufferInfo.colorFormats.empty() && nvrhi::getFormatInfo(framebufferInfo.colorFormats[0]).isSRGB;

    if (!BuildLayouts() || !BuildPipeline(framebufferInfo) || !EnsureInstanceCapacity(kInitialInstanceCapacity))
    {
        return false;
    }

    _white.UploadSolidColor(device, kWhiteTexel, kWhiteTexel, kWhiteTexel, kWhiteTexel, Image::ColorSpace::Linear,
                            "Mondrian::White");
    if (!_white.IsValid() || RegisterTexture(_white.NativeTexture()) != kWhiteTexture)
    {
        Core::Log::Error("Mondrian: the white texture could not be created.");
        return false;
    }
    return true;
}

bool QuadPass::BuildLayouts()
{
    // The instance buffer and the texture change for different reasons — growth
    // and draw entries — so each has its own set and neither rebuilds the other.
    nvrhi::BindingLayoutDesc instanceDesc;
    instanceDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    instanceDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PassConstants)));
    instanceDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    _instanceLayout = _device->createBindingLayout(instanceDesc);

    nvrhi::BindingLayoutDesc textureDesc;
    textureDesc.visibility = nvrhi::ShaderType::Pixel;
    textureDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    textureDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _textureLayout = _device->createBindingLayout(textureDesc);

    _sampler = _device->createSampler(
        nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));

    if (!_instanceLayout || !_textureLayout || !_sampler)
    {
        Core::Log::Error("Mondrian: the quad binding layouts could not be created.");
        return false;
    }
    return true;
}

bool QuadPass::BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS       = _vertexShader;
    pipelineDesc.PS       = _pixelShader;
    // Added in BindingSet order, which is the set number the shader declares.
    pipelineDesc.addBindingLayout(_instanceLayout);
    pipelineDesc.addBindingLayout(_textureLayout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    // The UI sits over everything the scene drew, whatever depth it left.
    pipelineDesc.renderState.depthStencilState.depthTestEnable  = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
    // Premultiplied: the fragment shader has already scaled colour by alpha.
    pipelineDesc.renderState.blendState.targets[0]
        .setBlendEnable(true)
        .setSrcBlend(nvrhi::BlendFactor::One)
        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

    _pipeline = _device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (!_pipeline)
    {
        Core::Log::Error("Mondrian: the quad pipeline could not be created.");
        return false;
    }
    return true;
}

bool QuadPass::EnsureInstanceCapacity(uint32_t instanceCount)
{
    if (_instanceBuffer && instanceCount <= _instanceCapacity)
    {
        return true;
    }

    // Doubling, so a screen that keeps gaining quads reallocates rarely.
    uint32_t capacity = _instanceCapacity == 0 ? kInitialInstanceCapacity : _instanceCapacity;
    while (capacity < instanceCount)
    {
        capacity *= 2;
    }

    nvrhi::BufferDesc desc;
    desc.byteSize         = static_cast<uint64_t>(capacity) * sizeof(QuadInstance);
    desc.structStride     = sizeof(QuadInstance);
    desc.initialState     = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName        = "Mondrian::Instances";
    nvrhi::BufferHandle grown = _device->createBuffer(desc);
    if (!grown)
    {
        Core::Log::Error("Mondrian: the instance buffer could not grow to {} quads.", capacity);
        return false;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(PassConstants)));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, grown));
    nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _instanceLayout);
    if (!set)
    {
        Core::Log::Error("Mondrian: the instance binding set could not be created.");
        return false;
    }

    _instanceBuffer   = grown;
    _instanceSet      = set;
    _instanceCapacity = capacity;
    return true;
}

TextureId QuadPass::RegisterTexture(nvrhi::ITexture *texture)
{
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
    nvrhi::BindingSetHandle set = texture != nullptr ? _device->createBindingSet(setDesc, _textureLayout) : nullptr;
    if (!set)
    {
        // White rather than a hole: a quad that draws plain is easier to notice
        // and cheaper to survive than one that faults.
        Core::Log::Error("Mondrian: a texture could not be registered; it draws white.");
        return kWhiteTexture;
    }
    _textureSets.push_back(set);
    return TextureId{static_cast<uint32_t>(_textureSets.size() - 1)};
}

void QuadPass::Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, const DrawList &drawList)
{
    ASSISI_ASSERT(drawList.IsFinalized(), "QuadPass::Draw given a DrawList that was never finalized");
    const std::span<const QuadInstance> instances = drawList.Instances();
    if (!_pipeline || instances.empty() || !EnsureInstanceCapacity(static_cast<uint32_t>(instances.size())))
    {
        return;
    }

    commandList->writeBuffer(_instanceBuffer, instances.data(), instances.size_bytes());

    const nvrhi::FramebufferInfoEx &target = framebuffer->getFramebufferInfo();
    const PassConstants constants{
        .viewport   = {static_cast<float>(target.width), static_cast<float>(target.height)},
        .encodeSrgb = _encodeSrgb ? 1u : 0u};

    nvrhi::GraphicsState state;
    state.pipeline    = _pipeline;
    state.framebuffer = framebuffer;
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(target.width), static_cast<float>(target.height)));

    for (const DrawEntry &entry : drawList.Entries())
    {
        ASSISI_ASSERT(entry.texture.value < _textureSets.size(), "a draw names a texture the pass never registered");
        const std::size_t textureIndex =
            entry.texture.value < _textureSets.size() ? entry.texture.value : kWhiteTexture.value;

        state.bindings.resize(static_cast<std::size_t>(BindingSet::Count));
        state.bindings[static_cast<std::size_t>(BindingSet::Instances)] = _instanceSet;
        state.bindings[static_cast<std::size_t>(BindingSet::Texture)]   = _textureSets[textureIndex];
        commandList->setGraphicsState(state);
        commandList->setPushConstants(&constants, sizeof(constants));

        nvrhi::DrawArguments drawArgs;
        drawArgs.vertexCount           = kVerticesPerQuad;
        drawArgs.instanceCount         = entry.instanceCount;
        drawArgs.startInstanceLocation = entry.firstInstance;
        commandList->draw(drawArgs);
    }
}

void QuadPass::Shutdown()
{
    // Sets before what they reference.
    _textureSets.clear();
    _instanceSet    = nullptr;
    _instanceBuffer = nullptr;
    _white          = Render::Texture{};
    _pipeline       = nullptr;
    _sampler        = nullptr;
    _textureLayout  = nullptr;
    _instanceLayout = nullptr;
    _pixelShader    = nullptr;
    _vertexShader   = nullptr;
    _instanceCapacity = 0;
}

void UploadPlaceholderTexture(Render::Texture &texture, nvrhi::IDevice *device)
{
    Image::DecodedImage image;
    image.width      = kCheckerSide;
    image.height     = kCheckerSide;
    image.format     = Image::PixelFormat::Rgba8;
    image.colorSpace = Image::ColorSpace::Linear;

    std::vector<unsigned char> &pixels = image.mips.emplace_back();
    pixels.reserve(static_cast<std::size_t>(kCheckerSide) * kCheckerSide * kRgba8Bytes);
    for (uint32_t y = 0; y < kCheckerSide; ++y)
    {
        for (uint32_t x = 0; x < kCheckerSide; ++x)
        {
            const bool light = ((x / kCheckerSquareSide) + (y / kCheckerSquareSide)) % 2 == 0;
            const std::array<unsigned char, 4> &texel = light ? kCheckerLight : kCheckerDark;
            pixels.insert(pixels.end(), texel.begin(), texel.end());
        }
    }
    texture.UploadDecoded(device, image, "Mondrian::Placeholder");
}

} // namespace Assisi::Mondrian::Engine
