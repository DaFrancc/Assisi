/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/SampleCountPipelines.hpp>

#include <bit>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Render
{

nvrhi::IGraphicsPipeline *SampleCountPipelines::For(nvrhi::IDevice *device, const nvrhi::GraphicsPipelineDesc &desc,
                                                    const nvrhi::FramebufferInfo &targets)
{
    const uint32_t samples = targets.sampleCount;
    const std::size_t slot = static_cast<std::size_t>(std::countr_zero(samples));
    if (device == nullptr || !std::has_single_bit(samples) || slot >= kSlots)
    {
        return nullptr;
    }
    const uint32_t bit = 1u << slot;
    if (_pipelines[slot] == nullptr && (_failed & bit) == 0u)
    {
        _pipelines[slot] = device->createGraphicsPipeline(desc, targets);
        if (_pipelines[slot] == nullptr)
        {
            Core::Log::Error("{}: failed to create the pipeline for {}x MSAA targets.", _name, samples);
            _failed |= bit;
        }
    }
    return _pipelines[slot];
}

void SampleCountPipelines::Clear()
{
    _pipelines.fill(nullptr);
    _failed = 0u;
}

} // namespace Assisi::Render
