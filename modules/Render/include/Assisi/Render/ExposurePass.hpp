/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ExposurePass.hpp
/// @brief Automatic exposure on the GPU: the scene's brightness histogram,
/// then the metering and adapting, which leave the exposure in a buffer the
/// tone map reads. Nothing comes back to the CPU. See Exposure.hpp.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ComputeShader.hpp>
#include <Assisi/Render/Exposure.hpp>

#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

class ExposurePass
{
public:
    /// @brief Loads the two shaders and makes the histogram and the kept
    /// exposure.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const std::string &histogramShaderSpvPath,
                                  const std::string &adaptShaderSpvPath);

    /// @brief Reads the histogram from @p scene, @p width by @p height pixels.
    void Configure(nvrhi::ITexture *scene, uint32_t width, uint32_t height);

    /// @brief The kept exposure, an ExposureState, for the tone map to read.
    [[nodiscard]] nvrhi::IBuffer *State() const { return _state; }

    /// @brief Meters this frame, which took @p seconds, and adapts the kept
    /// exposure toward @p settings' aim. Configure first.
    void Run(nvrhi::ICommandList *commandList, const ExposureSettings &settings, float seconds);

    void Shutdown();

private:
    ComputeShader _histogram;
    ComputeShader _adapt;
    nvrhi::IDevice *_device = nullptr;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BufferHandle _bins;
    nvrhi::BufferHandle _state;
    nvrhi::BindingSetHandle _histogramBindings;
    nvrhi::BindingSetHandle _adaptBindings;
    glm::uvec2 _sceneExtent{0u};
    /// Whether the kept exposure has been cleared, which marks it unseeded.
    bool _stateCleared = false;
};

} // namespace Assisi::Render
