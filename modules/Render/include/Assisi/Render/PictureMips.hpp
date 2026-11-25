/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PictureMips.hpp
/// @brief Draws the smaller levels of a picture a surface shows, each from the
/// one above it, so the picture doesn't shimmer on a surface smaller on screen
/// than the picture itself.

#include <cstdint>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief How many levels a @p width by @p height picture has, down to one
/// pixel on its longer side.
[[nodiscard]] uint32_t PictureMipCount(uint32_t width, uint32_t height);

/// @brief Draws a picture's levels below the first, each pixel the average of
/// the four above it.
class PictureMips
{
public:
    /// @brief The draws into one picture: a target and a source per level
    /// below the first. Made once per picture, since each names its texture.
    struct Chain
    {
        struct Level
        {
            nvrhi::FramebufferHandle framebuffer;
            nvrhi::BindingSetHandle bindings;
            uint32_t width = 0;
            uint32_t height = 0;
        };
        std::vector<Level> levels;
    };

    /// @brief Loads the shaders and builds the pipeline for pictures in
    /// @p format. False, logged, when any of it fails.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, nvrhi::Format format, const std::string &vertexSpvPath,
                                  const std::string &pixelSpvPath);

    [[nodiscard]] bool IsValid() const { return _pipeline != nullptr; }

    /// @brief The draws for @p picture's levels. Empty, logged, when they
    /// cannot be made, or when the picture has one level.
    [[nodiscard]] Chain Prepare(nvrhi::ITexture *picture) const;

    /// @brief Draws every level of @p chain from the one above it, after the
    /// first level was drawn.
    void Build(nvrhi::ICommandList *commandList, const Chain &chain) const;

private:
    nvrhi::IDevice *_device = nullptr;
    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::GraphicsPipelineHandle _pipeline;
};

} // namespace Assisi::Render
