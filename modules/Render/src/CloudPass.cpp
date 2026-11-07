/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/CloudPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuMarker.hpp>

#include <algorithm>
#include <bit>
#include <utility>

namespace Assisi::Render
{
namespace
{
// Must match the shaders' layout(binding = …), less the backend's offsets.
enum class NoiseSlot : uint32_t
{
    Shape = 0,
    Detail = 1,
    Weather = 2,
};
constexpr uint32_t kConstantsSlot = 0u;
constexpr uint32_t kSamplerSlot = 0u;
constexpr uint32_t kDestinationSlot = 0u;
constexpr uint32_t kSourceSlot = 0u;

// The noise shaders' workgroups are 4 texels on a side, the maps' 8.
constexpr uint32_t kNoiseGroupSize = 4u;
constexpr uint32_t kMapGroupSize = 8u;

struct NoisePush
{
    uint32_t size = 0;
    uint32_t kind = 0;
    uint32_t padding0 = 0;
    uint32_t padding1 = 0;
};

struct DownsamplePush
{
    uint32_t destinationSize = 0;
    uint32_t padding0 = 0;
    uint32_t padding1 = 0;
    uint32_t padding2 = 0;
};

uint32_t Groups(uint32_t texels, uint32_t groupSize)
{
    return (texels + groupSize - 1u) / groupSize;
}

nvrhi::BufferHandle CreateConstants(nvrhi::IDevice *device, size_t size, const char *name)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = size;
    desc.isConstantBuffer = true;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

nvrhi::TextureHandle CreateTarget(nvrhi::IDevice *device, uint32_t width, uint32_t height, nvrhi::Format format,
                                  const char *name)
{
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.isUAV = true;
    // Written by compute and read by the passes after it in the same command
    // list; nvrhi's automatic barriers move it between the two.
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = name;
    return device->createTexture(desc);
}
} // namespace

bool CloudPass::Initialize(const InitParams &params)
{
    _device = params.device;

    nvrhi::BindingLayoutDesc noiseLayout;
    noiseLayout.visibility = nvrhi::ShaderType::Compute;
    noiseLayout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(kDestinationSlot));
    noiseLayout.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(NoisePush)));
    if (!_noise.Initialize(_device, params.noiseSpvPath, noiseLayout))
    {
        return false;
    }

    nvrhi::BindingLayoutDesc downsampleLayout;
    downsampleLayout.visibility = nvrhi::ShaderType::Compute;
    downsampleLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(kSourceSlot));
    downsampleLayout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(kDestinationSlot));
    downsampleLayout.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DownsamplePush)));
    if (!_downsample.Initialize(_device, params.noiseDownsampleSpvPath, downsampleLayout))
    {
        return false;
    }

    // The weather pass reads the noise and writes the weather texture; the sky
    // map and the shadow read both through one layout.
    nvrhi::BindingLayoutDesc weatherLayout;
    weatherLayout.visibility = nvrhi::ShaderType::Compute;
    weatherLayout.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(kConstantsSlot));
    weatherLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(std::to_underlying(NoiseSlot::Shape)));
    weatherLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(std::to_underlying(NoiseSlot::Detail)));
    weatherLayout.addItem(nvrhi::BindingLayoutItem::Sampler(kSamplerSlot));
    weatherLayout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(kDestinationSlot));
    nvrhi::BindingLayoutDesc marchLayout = weatherLayout;
    marchLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(std::to_underlying(NoiseSlot::Weather)));
    if (!_weather.Initialize(_device, params.weatherSpvPath, weatherLayout) ||
        !_shadow.Initialize(_device, params.shadowSpvPath, marchLayout) ||
        !_map.Initialize(_device, params.mapSpvPath, marchLayout))
    {
        return false;
    }

    nvrhi::BindingLayoutDesc resolveLayout;
    resolveLayout.visibility = nvrhi::ShaderType::Compute;
    resolveLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(kSourceSlot));
    resolveLayout.addItem(nvrhi::BindingLayoutItem::Texture_UAV(kDestinationSlot));
    if (!_mapResolve.Initialize(_device, params.mapResolveSpvPath, resolveLayout))
    {
        _map = ComputeShader{};
        return false;
    }

    nvrhi::SamplerDesc samplerDesc;
    // Trilinear, so a sample reads the mip its footprint asks for and lands
    // between texels smoothly; wrapping, because the noise tiles everywhere.
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
    _noiseSampler = _device->createSampler(samplerDesc);

    _mapConstants = CreateConstants(_device, sizeof(CloudMapConstants), "CloudPass::MapConstants");
    _shadowConstants = CreateConstants(_device, sizeof(CloudShadowMapConstants), "CloudPass::ShadowConstants");
    if (_noiseSampler == nullptr || _mapConstants == nullptr || _shadowConstants == nullptr)
    {
        Core::Log::Error("CloudPass: failed to create its sampler and constant buffers.");
        _map = ComputeShader{};
        return false;
    }
    return true;
}

bool CloudPass::CreateNoise(Noise &noise, uint32_t size, const char *name)
{
    const uint32_t mipCount = static_cast<uint32_t>(std::bit_width(size));
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture3D;
    desc.width = size;
    desc.height = size;
    desc.depth = size;
    desc.mipLevels = mipCount;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = name;
    noise.texture = _device->createTexture(desc);
    noise.size = size;
    noise.mipSets.clear();
    if (noise.texture == nullptr)
    {
        Core::Log::Error("CloudPass: failed to allocate the {}^3 noise texture '{}'.", size, name);
        return false;
    }

    for (uint32_t mip = 0; mip < mipCount; ++mip)
    {
        nvrhi::BindingSetDesc setDesc;
        const ComputeShader &shader = mip == 0 ? _noise : _downsample;
        if (mip > 0)
        {
            setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(kSourceSlot, noise.texture, nvrhi::Format::UNKNOWN,
                                                               nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 1)));
        }
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(kDestinationSlot, noise.texture, nvrhi::Format::UNKNOWN,
                                                           nvrhi::TextureSubresourceSet(mip, 1, 0, 1),
                                                           nvrhi::TextureDimension::Texture3D));
        setDesc.addItem(nvrhi::BindingSetItem::PushConstants(
                            0, static_cast<uint32_t>(mip == 0 ? sizeof(NoisePush) : sizeof(DownsamplePush))));
        nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, shader.BindingLayout());
        if (set == nullptr)
        {
            Core::Log::Error("CloudPass: failed to create the binding set for mip {} of '{}'.", mip, name);
            return false;
        }
        noise.mipSets.push_back(std::move(set));
    }
    return true;
}

bool CloudPass::CreateMapBindingSets()
{
    // The weather pass reads the map's constants, whose first member is the
    // clouds' own block.
    nvrhi::BindingSetDesc weatherDesc;
    weatherDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(kConstantsSlot, _mapConstants));
    weatherDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(std::to_underlying(NoiseSlot::Shape), _shape.texture));
    weatherDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(std::to_underlying(NoiseSlot::Detail), _detail.texture));
    weatherDesc.addItem(nvrhi::BindingSetItem::Sampler(kSamplerSlot, _noiseSampler));
    weatherDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(kDestinationSlot, _weatherTexture));
    _weatherSet = _device->createBindingSet(weatherDesc, _weather.BindingLayout());

    nvrhi::BindingSetDesc mapDesc = weatherDesc;
    mapDesc.bindings[4] = nvrhi::BindingSetItem::Texture_UAV(kDestinationSlot, _marchTexture);
    mapDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(std::to_underlying(NoiseSlot::Weather), _weatherTexture));
    _mapSet = _device->createBindingSet(mapDesc, _map.BindingLayout());

    nvrhi::BindingSetDesc resolveDesc;
    resolveDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(kSourceSlot, _marchTexture));
    resolveDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(kDestinationSlot, _mapTexture));
    _resolveSet = _device->createBindingSet(resolveDesc, _mapResolve.BindingLayout());

    nvrhi::BindingSetDesc shadowDesc = mapDesc;
    shadowDesc.bindings[0] = nvrhi::BindingSetItem::ConstantBuffer(kConstantsSlot, _shadowConstants);
    shadowDesc.bindings[4] = nvrhi::BindingSetItem::Texture_UAV(kDestinationSlot, _shadowTexture);
    _shadowSet = _device->createBindingSet(shadowDesc, _shadow.BindingLayout());

    if (_weatherSet == nullptr || _mapSet == nullptr || _resolveSet == nullptr || _shadowSet == nullptr)
    {
        Core::Log::Error("CloudPass: failed to create the weather, sky and shadow maps' binding sets.");
        return false;
    }
    return true;
}

bool CloudPass::Configure(const CloudQuality &rawQuality)
{
    const CloudQuality quality = Sanitized(rawQuality);
    const bool mapFits = _mapTexture != nullptr && quality.resolution == _quality.resolution;
    _quality = quality;
    if (mapFits)
    {
        return true;
    }

    if (_shape.texture == nullptr &&
        (!CreateNoise(_shape, kCloudShapeNoiseSize, "CloudPass::ShapeNoise") ||
         !CreateNoise(_detail, kCloudDetailNoiseSize, "CloudPass::DetailNoise")))
    {
        Release();
        return false;
    }
    if (_shadowTexture == nullptr)
    {
        _shadowTexture = CreateTarget(_device, kCloudShadowResolution, kCloudShadowResolution,
                                      nvrhi::Format::R16_FLOAT, "CloudPass::Shadow");
    }
    if (_weatherTexture == nullptr)
    {
        _weatherTexture = CreateTarget(_device, kCloudWeatherResolution, kCloudWeatherResolution,
                                       nvrhi::Format::RGBA8_UNORM, "CloudPass::Weather");
    }
    _marchTexture = CreateTarget(_device, quality.resolution, quality.resolution / kCloudMapAspect,
                                 nvrhi::Format::RGBA16_FLOAT, "CloudPass::March");
    _mapTexture = CreateTarget(_device, quality.resolution, quality.resolution / kCloudMapAspect,
                               nvrhi::Format::RGBA16_FLOAT, "CloudPass::Map");
    if (_marchTexture == nullptr || _mapTexture == nullptr || _shadowTexture == nullptr ||
        _weatherTexture == nullptr)
    {
        Core::Log::Error("CloudPass: failed to allocate the {}x{} sky map, the shadow map or the weather map.",
                         quality.resolution, quality.resolution / kCloudMapAspect);
        Release();
        return false;
    }
    if (!CreateMapBindingSets())
    {
        Release();
        return false;
    }
    return true;
}

void CloudPass::Release()
{
    _shape = Noise{};
    _detail = Noise{};
    _noiseDrawn = false;
    _weatherSet = nullptr;
    _mapSet = nullptr;
    _resolveSet = nullptr;
    _shadowSet = nullptr;
    _weatherTexture = nullptr;
    _marchTexture = nullptr;
    _mapTexture = nullptr;
    _shadowTexture = nullptr;
}

void CloudPass::DrawNoise(nvrhi::ICommandList *commandList, const Noise &noise, CloudNoiseKind kind)
{
    const NoisePush push{.size = noise.size, .kind = std::to_underlying(kind)};
    const uint32_t groups = Groups(noise.size, kNoiseGroupSize);
    _noise.Dispatch(commandList, noise.mipSets[0], groups, groups, groups, &push, sizeof(push));
    for (uint32_t mip = 1; mip < noise.mipSets.size(); ++mip)
    {
        const DownsamplePush downsample{.destinationSize = std::max(noise.size >> mip, 1u)};
        const uint32_t mipGroups = Groups(downsample.destinationSize, kNoiseGroupSize);
        _downsample.Dispatch(commandList, noise.mipSets[mip], mipGroups, mipGroups, mipGroups, &downsample,
                             sizeof(downsample));
    }
}

void CloudPass::Render(nvrhi::ICommandList *commandList, const CloudMapConstants &map,
                       const CloudShadowMapConstants &shadow)
{
    if (_mapSet == nullptr)
    {
        return;
    }
    if (!_noiseDrawn)
    {
        ASSISI_PROFILE_GPU_SCOPE(commandList, "cloud-noise");
        DrawNoise(commandList, _shape, CloudNoiseKind::Shape);
        DrawNoise(commandList, _detail, CloudNoiseKind::Detail);
        _noiseDrawn = true;
    }

    // The weather first: the shadow and the sky map both read it.
    commandList->writeBuffer(_mapConstants, &map, sizeof(map));
    _weather.Dispatch(commandList, _weatherSet, Groups(kCloudWeatherResolution, kMapGroupSize),
                      Groups(kCloudWeatherResolution, kMapGroupSize), 1);

    commandList->writeBuffer(_shadowConstants, &shadow, sizeof(shadow));
    _shadow.Dispatch(commandList, _shadowSet, Groups(kCloudShadowResolution, kMapGroupSize),
                     Groups(kCloudShadowResolution, kMapGroupSize), 1);

    const uint32_t height = _quality.resolution / kCloudMapAspect;
    _map.Dispatch(commandList, _mapSet, Groups(_quality.resolution, kMapGroupSize), Groups(height, kMapGroupSize), 1);
    _mapResolve.Dispatch(commandList, _resolveSet, Groups(_quality.resolution, kMapGroupSize),
                         Groups(height, kMapGroupSize), 1);
}

} // namespace Assisi::Render
