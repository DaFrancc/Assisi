/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/SkyProbe.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/Sky.hpp>
#include <Assisi/Render/SkyPass.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::Render
{

namespace
{
// Mirrors sky_prefilter.comp's PushConstants block member for member.
struct PrefilterPush
{
    uint32_t faceSize;
    float roughness;
    uint32_t sampleCount;
    uint32_t padding;
};
static_assert(sizeof(PrefilterPush) == 4 * sizeof(uint32_t), "PrefilterPush must match sky_prefilter.comp.");

// sky_prefilter.comp's local_size_x and local_size_y.
constexpr uint32_t kPrefilterGroupSize = 8;

// Half-float radiance: the sky runs from a night floor four orders below the
// daytime zenith to a horizon an order above it, which is range an 8-bit
// target cannot hold and precision a 32-bit one would waste.
constexpr nvrhi::Format kProbeFormat = nvrhi::Format::RGBA16_FLOAT;

uint32_t GroupCount(uint32_t texels)
{
    return (texels + kPrefilterGroupSize - 1u) / kPrefilterGroupSize;
}
} // namespace

bool SkyProbe::Initialize(nvrhi::IDevice *device, const std::string &prefilterSpvPath)
{
    _device = device;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PrefilterPush)));
    if (!_prefilter.Initialize(device, prefilterSpvPath, layoutDesc))
    {
        return false;
    }

    // Linear, so a sample landing between texels reads the sky between them
    // rather than the nearest one. Cube sampling in Vulkan is seamless whatever
    // the address mode, so clamping only matters to a sampler used elsewhere.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sourceSampler = device->createSampler(samplerDesc);
    if (_sourceSampler == nullptr)
    {
        Core::Log::Error("SkyProbe: failed to create the capture sampler.");
        return false;
    }
    return true;
}

bool SkyProbe::Configure(const EnvironmentSettings &settings)
{
    const EnvironmentSettings sanitized = Sanitized(settings);
    const bool resized = _capture == nullptr || sanitized.resolution != _resolution;
    const bool resampled = sanitized.sampleCount != _settings.sampleCount;
    _settings = sanitized;

    if (resized)
    {
        if (!AllocateTargets(sanitized.resolution))
        {
            Release();
            return false;
        }
        _baked = false;
        _stats = Stats{};
    }
    else if (resampled)
    {
        // A different sample count is a different estimate of the same sky,
        // and the one held was made with the old count.
        _baked = false;
    }
    return true;
}

void SkyProbe::Release()
{
    _capture = nullptr;
    _prefiltered = nullptr;
    _faceFramebuffers = {};
    _mipBindingSets.clear();
    _resolution = 0;
    _mipCount = 0;
    _baked = false;
    _stats = Stats{};
}

bool SkyProbe::AllocateTargets(uint32_t resolution)
{
    Release();

    nvrhi::TextureDesc captureDesc;
    captureDesc.width = resolution;
    captureDesc.height = resolution;
    captureDesc.arraySize = kCubeFaceCount;
    captureDesc.dimension = nvrhi::TextureDimension::TextureCube;
    captureDesc.format = kProbeFormat;
    captureDesc.isRenderTarget = true;
    // Drawn into and then read in the same command list; nvrhi's automatic
    // barriers move it between the two and restore this on the way out.
    captureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    captureDesc.keepInitialState = true;
    captureDesc.debugName = "SkyProbe::Capture";
    _capture = _device->createTexture(captureDesc);

    const uint32_t mipCount = SkyProbeMipCount(resolution);
    nvrhi::TextureDesc prefilteredDesc = captureDesc;
    prefilteredDesc.mipLevels = mipCount;
    prefilteredDesc.isRenderTarget = false;
    prefilteredDesc.isUAV = true;
    prefilteredDesc.debugName = "SkyProbe::Prefiltered";
    _prefiltered = _device->createTexture(prefilteredDesc);

    if (_capture == nullptr || _prefiltered == nullptr)
    {
        Core::Log::Error("SkyProbe: failed to allocate the {}x{} probe cubes.", resolution, resolution);
        return false;
    }

    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        nvrhi::FramebufferDesc framebufferDesc;
        framebufferDesc.addColorAttachment(nvrhi::FramebufferAttachment().setTexture(_capture).setArraySlice(face));
        _faceFramebuffers[face] = _device->createFramebuffer(framebufferDesc);
        if (_faceFramebuffers[face] == nullptr)
        {
            Core::Log::Error("SkyProbe: failed to create the capture framebuffer for face {}.", face);
            return false;
        }
    }

    _mipBindingSets.reserve(mipCount);
    for (uint32_t mip = 0; mip < mipCount; ++mip)
    {
        // Each mip is written as six layers of a 2D array: a storage image
        // cannot be a cube, and the layers are the cube's faces in its order.
        nvrhi::BindingSetDesc setDesc;
        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, _capture));
        setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _sourceSampler));
        setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(0, _prefiltered, nvrhi::Format::UNKNOWN,
                                                           nvrhi::TextureSubresourceSet(mip, 1, 0, kCubeFaceCount),
                                                           nvrhi::TextureDimension::Texture2DArray));
        setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(PrefilterPush)));
        nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _prefilter.BindingLayout());
        if (set == nullptr)
        {
            Core::Log::Error("SkyProbe: failed to create the prefilter binding set for mip {}.", mip);
            return false;
        }
        _mipBindingSets.push_back(std::move(set));
    }

    _resolution = resolution;
    _mipCount = mipCount;
    return true;
}

bool SkyProbe::NeedsBake(const SkyProbeInputs &inputs, uint32_t jumpSerial) const
{
    return !_baked || jumpSerial != _bakedJumpSerial ||
           SkyProbeInputsDiffer(_bakedInputs, inputs, _settings.rebakeDegrees);
}

void SkyProbe::Bake(nvrhi::ICommandList *commandList, SkyPass &sky, const SkyProbeInputs &inputs,
                    uint32_t jumpSerial)
{
    if (_capture == nullptr || _prefiltered == nullptr)
    {
        return;
    }
    Capture(commandList, sky, inputs);
    Prefilter(commandList);

    _bakedInputs = inputs;
    _bakedJumpSerial = jumpSerial;
    _baked = true;
    ++_stats.bakes;
    _stats.ageFrames = 0;
    _stats.bakedThisFrame = true;
}

void SkyProbe::Keep()
{
    ++_stats.ageFrames;
    _stats.bakedThisFrame = false;
}

void SkyProbe::Capture(nvrhi::ICommandList *commandList, SkyPass &sky, const SkyProbeInputs &inputs)
{
    // From the cube's centre: the sky is at infinity, so where the eye stands
    // changes nothing but the direction, and the direction is all the face's
    // matrix supplies.
    const glm::vec3 centre(0.0f);
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        const SkyConstants constants = MakeSkyConstants(glm::inverse(CubeFaceViewProjection(face)), centre,
                                                        inputs.sun, inputs.moon, inputs.settings);
        sky.DrawInto(commandList, _faceFramebuffers[face], constants);
    }
}

void SkyProbe::Prefilter(nvrhi::ICommandList *commandList)
{
    for (uint32_t mip = 0; mip < _mipCount; ++mip)
    {
        const uint32_t faceSize = std::max(_resolution >> mip, 1u);
        const PrefilterPush push{.faceSize = faceSize,
                                 .roughness = PrefilterRoughness(mip, _mipCount),
                                 .sampleCount = PrefilterSampleCount(mip, _settings.sampleCount),
                                 .padding = 0};
        _prefilter.Dispatch(commandList, _mipBindingSets[mip], GroupCount(faceSize), GroupCount(faceSize),
                            kCubeFaceCount, &push, sizeof(push));
    }
}

} // namespace Assisi::Render
