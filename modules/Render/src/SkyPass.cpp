/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/SkyPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/AssetSource.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <cstdint>
#include <utility>

namespace Assisi::Render
{
namespace
{
// Where each constant block sits. Must match sky.frag's layout(binding = …),
// less the backend's constant-buffer offset.
enum class ConstantSlot : std::uint8_t
{
    Sky = 0,
    Fog = 1,
};

// The moon's texture and its sampler, each first in its own register space,
// and the clouds' after them.
constexpr uint32_t kMoonSlot = 0u;
constexpr uint32_t kCloudMapSlot = 1u;
} // namespace

bool SkyPass::Initialize(const InitParams &params)
{
    _device = params.device;

    _vertexShader = LoadSpirvShader(_device, params.vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = LoadSpirvShader(_device, params.pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        return false;
    }

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(SkyConstants);
    constantsDesc.isConstantBuffer = true;
    constantsDesc.debugName = "SkyPass::Constants";
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    _constantsBuffer = _device->createBuffer(constantsDesc);
    if (_constantsBuffer == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the constants buffer.");
        return false;
    }

    nvrhi::BufferDesc fogDesc;
    fogDesc.byteSize = sizeof(SkyFogConstants);
    fogDesc.isConstantBuffer = true;
    fogDesc.debugName = "SkyPass::FogConstants";
    fogDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    fogDesc.keepInitialState = true;
    _fogConstantsBuffer = _device->createBuffer(fogDesc);
    if (_fogConstantsBuffer == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the fog constants buffer.");
        return false;
    }
    _fogVolumeBuffers = params.fogVolumes;

    _moonTexturePath = params.moonTexturePath;

    // White, so a moon drawn before its photograph arrives — or one whose
    // photograph never arrives — is the flat tinted disk rather than a black
    // hole. Allocated here rather than lazily because a binding set has to name
    // a real texture from the first frame, and building one variant of the
    // pipeline for "has a moon" would double something to skip a branch the GPU
    // already skips uniformly.
    _moon.UploadSolidColor(_device, 255, 255, 255, 255, Image::ColorSpace::Srgb, "SkyPass::MoonPlaceholder");
    if (!_moon.IsValid())
    {
        Core::Log::Error("SkyPass: failed to create the moon placeholder texture.");
        return false;
    }

    nvrhi::SamplerDesc samplerDesc;
    // Trilinear: a half-degree disk is about nine pixels across at 1080p, which
    // is mip five or six of a 512 image, and the derivative-selected level is
    // what stops it sparkling as the camera turns.
    samplerDesc.setAllFilters(true);
    // Clamp, because outside the disk the coordinate runs past [0, 1] under a
    // profile already fading to zero. Repeating would drag the opposite limb in.
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _moonSampler = _device->createSampler(samplerDesc);
    if (_moonSampler == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the moon sampler.");
        return false;
    }

    _noClouds.UploadSolidColor(_device, 0, 0, 0, 255, Image::ColorSpace::Linear, "SkyPass::NoClouds");
    if (!_noClouds.IsValid())
    {
        Core::Log::Error("SkyPass: failed to create the empty cloud map.");
        return false;
    }
    nvrhi::SamplerDesc cloudSamplerDesc;
    cloudSamplerDesc.setAllFilters(true);
    // Across is the bearing, which comes round to where it started; up stops at
    // the horizon and the zenith.
    cloudSamplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    cloudSamplerDesc.setAddressU(nvrhi::SamplerAddressMode::Wrap);
    _cloudMapSampler = _device->createSampler(cloudSamplerDesc);
    if (_cloudMapSampler == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the cloud map sampler.");
        return false;
    }

    // Both stages read the block: the vertex shader for the inverse
    // view-projection, the pixel shader for everything else.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(std::to_underlying(ConstantSlot::Sky)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(std::to_underlying(ConstantSlot::Fog)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(kMoonSlot));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(kMoonSlot));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(kCloudMapSlot));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(kCloudMapSlot));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(
                           std::to_underlying(FogVolumeBuffers::Slot::Volumes)));
    layoutDesc.addItem(
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(FogVolumeBuffers::Slot::Edges)));
    layoutDesc.addItem(
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(FogVolumeBuffers::Slot::Tiles)));
    _bindingLayout = _device->createBindingLayout(layoutDesc);
    if (_bindingLayout == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky binding layout.");
        return false;
    }

    if (!BuildBindingSet())
    {
        return false;
    }

    return BuildPipeline(params.framebufferInfo);
}

bool SkyPass::BuildBindingSet()
{
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(std::to_underlying(ConstantSlot::Sky), _constantsBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(std::to_underlying(ConstantSlot::Fog), _fogConstantsBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(kMoonSlot, _moon.NativeTexture()));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(kMoonSlot, _moonSampler));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                        kCloudMapSlot, _cloudMap != nullptr ? _cloudMap : _noClouds.NativeTexture()));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(kCloudMapSlot, _cloudMapSampler));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(FogVolumeBuffers::Slot::Volumes),
                                                                _fogVolumeBuffers->Volumes().NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(FogVolumeBuffers::Slot::Edges),
                                                                _fogVolumeBuffers->Edges().NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(FogVolumeBuffers::Slot::Tiles),
                                                                _fogVolumeBuffers->Tiles().NativeBuffer()));
    _bindingSet = _device->createBindingSet(setDesc, _bindingLayout);
    if (_bindingSet == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky binding set.");
        return false;
    }
    return true;
}

void SkyPass::SetClouds(nvrhi::ITexture *cloudMap)
{
    if (cloudMap == _cloudMap)
    {
        return;
    }
    _cloudMap = cloudMap;
    if (_bindingLayout != nullptr)
    {
        (void)BuildBindingSet();
    }
}

void SkyPass::LoadMoonTexture()
{
    _moonState = MoonTexture::Failed;
    if (_moonTexturePath.empty())
    {
        Core::Log::Warn("SkyPass: no moon texture path; the moon is drawn as a flat disk.");
        return;
    }

    // Uncompressed is what is asked for; a source holding the texture already
    // block-compressed hands back that instead, and the upload takes either.
    const AssetSource *source = GetAssetSource();
    const std::expected<Core::AssetId, AssetLoadError> id =
        source != nullptr ? source->Resolve(_moonTexturePath) : std::unexpected(AssetLoadError::UnknownAsset);
    std::expected<Image::DecodedImage, AssetLoadError> image =
        id ? source->LoadTexture(*id, Image::ColorSpace::Srgb, Image::PixelFormat::Rgba8, /*alphaCutoff=*/ 0.f)
           : std::unexpected(id.error());

    Texture loaded;
    if (image)
    {
        loaded.UploadDecoded(_device, *image, _moonTexturePath.c_str());
    }
    if (!image || !loaded.IsValid())
    {
        Core::Log::Warn("SkyPass: could not load the moon texture '{}'; the moon is drawn as a flat disk.",
                        _moonTexturePath);
        return;
    }

    _moon = std::move(loaded);
    // The set names the texture, so replacing the texture means replacing the
    // set. The layout is unchanged, so the pipeline is not rebuilt.
    if (!BuildBindingSet())
    {
        return;
    }
    _moonState = MoonTexture::Loaded;
}

bool SkyPass::BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = _vertexShader;
    pipelineDesc.PS = _pixelShader;
    pipelineDesc.addBindingLayout(_bindingLayout);

    // No input layout and no vertex buffer: the triangle comes from
    // gl_VertexIndex. Culling is off because that triangle's winding is a
    // property of the index arithmetic rather than of anything a caller controls.
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;

    // Equal against the far-plane depth the clear left. The scene wrote a
    // nearer depth wherever it drew, which is what makes covered pixels fail
    // here and cost nothing. Depth is not written, so the scene's depth reaches
    // the passes after this one intact.
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
    pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Equal;

    _pipeline = _device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (_pipeline == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky pipeline.");
        return false;
    }
    return true;
}

bool SkyPass::RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (_bindingLayout == nullptr)
    {
        return true; // nothing built yet — nothing to rebuild
    }
    return BuildPipeline(framebufferInfo);
}

void SkyPass::Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &cameraPosition,
                   const SkySun &sun, const SkyMoon &moon, const SkySettings &settings)
{
    if (!IsValid())
    {
        return;
    }

    ASSISI_PROFILE_GPU_PASS(frame.commandList, "sky");

    // Pay for what you place: the file is opened on the first frame something
    // asks for a moon disk, and never at all in a level without one.
    if (moon.diskIntensity > 0.0f && _moonState == MoonTexture::NotLoaded)
    {
        LoadMoonTexture();
    }

    SkyConstants constants = MakeSkyConstants(glm::inverse(viewProjection), cameraPosition, sun, moon, settings);
    constants.fog = _fog;
    Submit(frame.commandList, frame.framebuffer, _pipeline,
           nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)), constants,
           _fogVolumes);
}

void SkyPass::DrawInto(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *target, const SkyConstants &constants)
{
    if (!IsValid() || target == nullptr)
    {
        return;
    }

    if (_capturePipeline == nullptr)
    {
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.VS = _vertexShader;
        pipelineDesc.PS = _pixelShader;
        pipelineDesc.addBindingLayout(_bindingLayout);
        pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        // No depth to test against: every texel of a capture target is sky.
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
        _capturePipeline = _device->createGraphicsPipeline(pipelineDesc, target->getFramebufferInfo());
        if (_capturePipeline == nullptr)
        {
            Core::Log::Error("SkyPass: failed to create the sky capture pipeline.");
            return;
        }
    }
    const nvrhi::FramebufferInfoEx &info = target->getFramebufferInfo();
    Submit(commandList, target, _capturePipeline,
           nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)), constants,
           SkyFogConstants{});
}

void SkyPass::Submit(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *target,
                     nvrhi::IGraphicsPipeline *pipeline, const nvrhi::Viewport &viewport,
                     const SkyConstants &constants, const SkyFogConstants &fog)
{
    SkyConstants withClouds = constants;
    withClouds.atmosphere.y = _cloudMap != nullptr ? 1.0f : 0.0f;
    commandList->writeBuffer(_constantsBuffer, &withClouds, sizeof(withClouds));
    commandList->writeBuffer(_fogConstantsBuffer, &fog, sizeof(fog));

    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = target;
    state.addBindingSet(_bindingSet);
    state.viewport.addViewportAndScissorRect(viewport);
    commandList->setGraphicsState(state);

    // One triangle covering the target; sky.vert makes it from the index.
    constexpr uint32_t kFullscreenTriangleVertices = 3;
    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = kFullscreenTriangleVertices;
    commandList->draw(drawArgs);
}

} // namespace Assisi::Render
