/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PictureLevels.hpp
/// @brief Draws the levels of a picture a surface shows: the first stretched
/// from the part of a target a camera drew, each smaller one from the one
/// above it, so the picture fills its surface whatever size the camera drew at
/// and doesn't shimmer on a surface smaller on screen than the picture itself.

#include <cstdint>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{

/// @brief How many levels a @p width by @p height picture has, down to one
/// pixel on its longer side.
[[nodiscard]] uint32_t PictureMipCount(uint32_t width, uint32_t height);

/// @brief How a picture darkens toward its edge, as a rifle scope's does when
/// the eye is off its axis: clear inside a circle, fading over a band past it,
/// and as dark as the strength says beyond. Distances are in picture heights,
/// so the circle stays round on a wide picture.
struct Vignette
{
    /// The clear circle's middle, in the picture's own coordinates: 0 to 1
    /// across and down from its top-left corner.
    glm::vec2 centre{0.5f, 0.5f};
    /// How far from the centre the picture stays clear.
    float radius = 0.f;
    /// How wide the fade from clear to dark is, past the radius.
    float softness = 0.f;
    /// How dark it gets beyond the fade: 0 for no darkening, 1 for black.
    float strength = 0.f;

    [[nodiscard]] bool operator==(const Vignette &) const = default;
};

/// @brief Draws a picture's levels: the first from what a camera drew, each
/// pixel of the others the average of the four above it, or close to it where
/// a side of the level above is odd.
class PictureLevels
{
public:
    /// @brief One draw into a level, and what it reads.
    struct Level
    {
        nvrhi::FramebufferHandle framebuffer;
        nvrhi::BindingSetHandle bindings;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    /// @brief The draws into one picture: the first level from the target the
    /// camera drew into, then each level below from the one above. Made once
    /// per picture, since each names its textures.
    struct Chain
    {
        std::vector<Level> levels;
    };

    /// @brief Loads the shaders and builds the pipeline for pictures in
    /// @p format. False, logged, when any of it fails.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, nvrhi::Format format, const std::string &vertexSpvPath,
                                  const std::string &pixelSpvPath);

    [[nodiscard]] bool IsValid() const { return _pipeline != nullptr; }

    /// @brief The draws for @p picture's levels, the first read from
    /// @p drawn, a single-sample texture the picture's size. Empty, logged,
    /// when they cannot be made.
    [[nodiscard]] Chain Prepare(nvrhi::ITexture *picture, nvrhi::ITexture *drawn) const;

    /// @brief Stretches the top-left @p drawnWidth x @p drawnHeight of the
    /// drawn texture over the picture's first level, darkened by
    /// @p vignette, then draws every level below from the one above, so each
    /// carries the same darkening.
    void Build(nvrhi::ICommandList *commandList, const Chain &chain, uint32_t drawnWidth, uint32_t drawnHeight,
               const Vignette &vignette) const;

private:
    /// @brief What part of the level above a draw reads, in its texture
    /// coordinates, and how the level is darkened. Matches
    /// picture_levels.frag.
    struct Constants
    {
        float scaleX = 1.f;
        float scaleY = 1.f;
        float mostX = 1.f;
        float mostY = 1.f;
        float centreX = 0.f;
        float centreY = 0.f;
        float radius = 0.f;
        /// The fade's width, never zero so the shader never divides by it.
        float softness = 1.f;
        /// Zero on every level but the first, which leaves the others as read.
        float strength = 0.f;
        /// The level's width over its height, so distances are in heights.
        float aspect = 1.f;
    };

    void Draw(nvrhi::ICommandList *commandList, const Level &level, const Constants &constants) const;

    nvrhi::IDevice *_device = nullptr;
    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::GraphicsPipelineHandle _pipeline;
};

} // namespace Assisi::Render
