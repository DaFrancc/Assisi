/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ExposurePass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuMarker.hpp>

namespace Assisi::Render
{
namespace
{

// exposure_histogram.comp's workgroup is this many samples on a side.
constexpr uint32_t kHistogramGroupSide = 16u;

// The histogram samples once per block of this many pixels on a side.
constexpr uint32_t kSampleBlock = 2u;

nvrhi::BufferHandle MakeStorage(nvrhi::IDevice *device, uint32_t stride, uint32_t count, const char *name)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = static_cast<uint64_t>(stride) * count;
    desc.structStride = stride;
    desc.canHaveUAVs = true;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

} // namespace

bool ExposurePass::Initialize(nvrhi::IDevice *device, const std::string &histogramShaderSpvPath,
                              const std::string &adaptShaderSpvPath)
{
    _device = device;

    nvrhi::BindingLayoutDesc histogramLayout;
    histogramLayout.visibility = nvrhi::ShaderType::Compute;
    histogramLayout.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ExposureConstants)));
    histogramLayout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    histogramLayout.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    histogramLayout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));

    nvrhi::BindingLayoutDesc adaptLayout;
    adaptLayout.visibility = nvrhi::ShaderType::Compute;
    adaptLayout.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ExposureConstants)));
    adaptLayout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    adaptLayout.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));

    if (!_histogram.Initialize(_device, histogramShaderSpvPath, histogramLayout) ||
        !_adapt.Initialize(_device, adaptShaderSpvPath, adaptLayout))
    {
        return false;
    }

    // Bilinear, so one tap in the middle of a 2x2 block averages it.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _sampler = _device->createSampler(samplerDesc);

    _bins = MakeStorage(_device, sizeof(uint32_t), kExposureHistogramBins, "Exposure::Histogram");
    _state = MakeStorage(_device, sizeof(ExposureState), 1u, "Exposure::State");
    if (!_sampler || !_bins || !_state)
    {
        Core::Log::Error("Exposure: could not create its buffers.");
        return false;
    }

    nvrhi::BindingSetDesc adaptBindings;
    adaptBindings.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ExposureConstants)));
    adaptBindings.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, _bins));
    adaptBindings.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, _state));
    _adaptBindings = _device->createBindingSet(adaptBindings, _adapt.BindingLayout());
    return _adaptBindings != nullptr;
}

void ExposurePass::Configure(nvrhi::ITexture *scene, uint32_t width, uint32_t height)
{
    _histogramBindings = nullptr;
    _sceneExtent = glm::uvec2(width, height);
    if (_device == nullptr || scene == nullptr || width == 0u || height == 0u)
    {
        return;
    }
    nvrhi::BindingSetDesc bindings;
    bindings.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ExposureConstants)));
    bindings.addItem(nvrhi::BindingSetItem::Texture_SRV(0, scene));
    bindings.addItem(nvrhi::BindingSetItem::Sampler(0, _sampler));
    bindings.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, _bins));
    _histogramBindings = _device->createBindingSet(bindings, _histogram.BindingLayout());
}

void ExposurePass::Run(nvrhi::ICommandList *commandList, const ExposureSettings &settings, float seconds)
{
    if (!_histogramBindings || !_adaptBindings)
    {
        return;
    }
    ASSISI_PROFILE_GPU_SCOPE(commandList, "pp-exposure");

    if (!_stateCleared)
    {
        commandList->clearBufferUInt(_state, 0u);
        _stateCleared = true;
    }
    commandList->clearBufferUInt(_bins, 0u);

    const ExposureSettings safe = Sanitized(settings);
    const glm::uvec2 grid = glm::max(_sceneExtent / kSampleBlock, glm::uvec2(1u));
    const ExposureConstants constants{.sampleGrid = grid,
                                      .texelSize = 1.0f / glm::vec2(_sceneExtent),
                                      .minStops = safe.minStops,
                                      .maxStops = safe.maxStops,
                                      .compensationStops = safe.compensationStops,
                                      .adaptationRate = safe.adaptationRate,
                                      .seconds = seconds};
    const glm::uvec2 groups = (grid + glm::uvec2(kHistogramGroupSide - 1u)) / kHistogramGroupSide;
    _histogram.Dispatch(commandList, _histogramBindings, groups.x, groups.y, 1u, &constants, sizeof(constants));
    _adapt.Dispatch(commandList, _adaptBindings, 1u, 1u, 1u, &constants, sizeof(constants));
}

void ExposurePass::Shutdown()
{
    _histogramBindings = nullptr;
    _adaptBindings = nullptr;
    _state = nullptr;
    _bins = nullptr;
    _sampler = nullptr;
    _histogram = ComputeShader{};
    _adapt = ComputeShader{};
    _device = nullptr;
    _stateCleared = false;
}

} // namespace Assisi::Render
