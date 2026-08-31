/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyPass.hpp
/// @brief Fills the pixels the scene left empty with sky.

#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/Sky.hpp>

namespace Assisi::Render
{

/// @brief Draws the analytic sky behind the opaque scene.
///
/// One triangle covering the screen at depth 1.0, tested for EQUALITY against
/// the depth the scene wrote. The depth clear is 1.0 and the mesh pipeline tests
/// Less, so every pixel geometry covered holds something smaller and fails the
/// test outright — the sky is shaded on exactly the pixels that would otherwise
/// have kept the clear colour, and a screen full of geometry costs it nothing
/// but the depth reads.
///
/// Nothing is written to depth, so a pass drawing after this one sees the scene's
/// depth unchanged.
///
/// Run it AFTER the opaque geometry. Running it first would shade a whole screen
/// of sky for the geometry to paint over, which is the cost this arrangement
/// exists to avoid.
class SkyPass
{
public:
    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// Format/sample-count of the scene target the sky composites into,
        /// including its depth attachment.
        nvrhi::FramebufferInfo framebufferInfo;
        std::string vertexShaderSpvPath;
        std::string pixelShaderSpvPath;
    };

    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Rebuild the pipeline for a new scene render-target format (e.g. an
    /// MSAA toggle). Shaders and binding set are reused. No-op (true) before
    /// Initialize().
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);

    [[nodiscard]] bool IsValid() const { return _pipeline != nullptr; }

    /// @brief Shade the frame's empty pixels.
    ///
    /// @param viewProjection  The projection * view the scene was drawn with.
    ///        The pass inverts it to turn a clip-space corner back into a world
    ///        ray; inverting that exact matrix is what makes the sky line up
    ///        with the geometry through any viewport convention. Taken forward
    ///        rather than already inverted so the inverse is inside the pass's
    ///        own profiler slice instead of an unnamed gap at the call site.
    /// @param cameraPosition  World-space eye, which the world ray is measured
    ///        from.
    ///
    /// No-op if not initialised.
    void Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &cameraPosition,
              const SkySun &sun, const SkySettings &settings);

private:
    [[nodiscard]] bool BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::BufferHandle _constantsBuffer;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::BindingSetHandle _bindingSet;
    nvrhi::GraphicsPipelineHandle _pipeline;
};

} // namespace Assisi::Render
