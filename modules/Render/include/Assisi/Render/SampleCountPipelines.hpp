/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SampleCountPipelines.hpp
/// @brief One pipeline per MSAA sample count, for a pass that draws into views
/// whose targets differ only in how many samples a pixel has.

#include <array>
#include <cstdint>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief A pass's pipelines, one for each sample count it has drawn at.
///
/// A pipeline is built for one sample count. The main view draws at the game's
/// MSAA setting and a texture camera at its own, so a pass every view shares,
/// such as the sky or the water, keeps one of each, built the first time a view
/// with that count draws.
class SampleCountPipelines
{
public:
    /// @brief Pipelines for the pass @p name, which a failure names.
    explicit SampleCountPipelines(const char *name) : _name(name) {}

    /// @brief The pipeline @p desc builds for targets like @p targets, built
    /// on first use. Null when it failed to build, which is logged and not
    /// tried again until Clear, or when the sample count is not a power of two
    /// up to the largest Vulkan has.
    [[nodiscard]] nvrhi::IGraphicsPipeline *For(nvrhi::IDevice *device, const nvrhi::GraphicsPipelineDesc &desc,
                                                const nvrhi::FramebufferInfo &targets);

    /// @brief Drops every pipeline, for targets whose format changed.
    void Clear();

private:
    /// One slot per power-of-two sample count, 1 to 64.
    static constexpr std::size_t kSlots = 7u;

    std::array<nvrhi::GraphicsPipelineHandle, kSlots> _pipelines{};
    const char *_name;
    /// One bit per slot whose pipeline failed to build.
    uint32_t _failed = 0u;
};

} // namespace Assisi::Render
