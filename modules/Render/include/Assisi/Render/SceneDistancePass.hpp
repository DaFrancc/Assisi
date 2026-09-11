/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneDistancePass.hpp
/// @brief The depth prepass's depth as a distance per pixel: what every
/// screen-space feature reads.

#include <Assisi/Math/GLM.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

namespace Assisi::Render
{

/// @brief One fullscreen step from the scene's depth buffer to a single-sampled
/// target holding each pixel's distance from the camera in metres.
///
/// Its own pass rather than the first step of ambient occlusion, which is only
/// one reader of it and an optional one: a feature that needs the scene's
/// distance takes it from here rather than from inside another feature.
///
/// **Nothing exists until a frame asks for it.** Initialize builds the
/// pipelines; the target arrives with Configure and leaves with Release.
class SceneDistancePass
{
public:
    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// Shared fullscreen-triangle vertex stage.
        std::string vertexShaderSpvPath;
        std::string distanceShaderSpvPath;
        /// The same step reading a multisampled depth buffer.
        std::string multisampleDistanceShaderSpvPath;
    };

    /// @brief Loads the shaders and builds both pipelines. Allocates no target.
    [[nodiscard]] bool Initialize(const InitParams &params);

    [[nodiscard]] bool IsValid() const { return _pipeline != nullptr && _multisamplePipeline != nullptr; }

    /// @brief Hold a @p width x @p height target reading @p depth, rebuilding
    /// only what a changed size or a changed depth buffer invalidates.
    /// @return false if the target could not be allocated; the pass then holds
    /// nothing.
    [[nodiscard]] bool Configure(std::uint32_t width, std::uint32_t height, nvrhi::ITexture *depth);

    /// @brief Free the target and the set over it.
    void Release();

    /// @brief Record the step into @p commandList, for a camera projecting with
    /// @p projection. The result is in DistanceTexture() when it returns.
    /// @pre Configure succeeded.
    void Render(nvrhi::ICommandList *commandList, const glm::mat4 &projection) const;

    /// @brief Distance per pixel in metres; null while nothing is held. Its
    /// handle survives every Render, and changes only with a Configure that
    /// reallocates.
    [[nodiscard]] nvrhi::ITexture *DistanceTexture() const { return _distance; }

private:
    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::BindingLayoutHandle _layout;
    nvrhi::GraphicsPipelineHandle _pipeline;
    nvrhi::GraphicsPipelineHandle _multisamplePipeline;
    nvrhi::BufferHandle _constants;

    std::uint32_t _width = 0;
    std::uint32_t _height = 0;
    nvrhi::ITexture *_depth = nullptr;

    nvrhi::TextureHandle _distance;
    nvrhi::FramebufferHandle _framebuffer;
    nvrhi::BindingSetHandle _bindingSet;
};

} // namespace Assisi::Render
