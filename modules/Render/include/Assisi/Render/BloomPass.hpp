/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BloomPass.hpp
/// @brief The bloom's chain of copies on the GPU: down from the scene, then
/// back up to the first copy, which the tone map mixes in. See Bloom.hpp.

#include <Assisi/Render/Bloom.hpp>

#include <array>
#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief The format of every copy: float, since the copies hold radiance.
inline constexpr nvrhi::Format kBloomFormat = nvrhi::Format::RGBA16_FLOAT;

class BloomPass
{
public:
    /// @brief Loads the two shaders and builds what does not depend on the
    /// screen. @p fullscreenVertexShader is the post chain's.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, nvrhi::IShader *fullscreenVertexShader,
                                  const std::string &downShaderSpvPath, const std::string &upShaderSpvPath);

    /// @brief Builds the copies for a @p width by @p height screen, read from
    /// @p source, or drops them for a zero size or no source.
    void Configure(uint32_t width, uint32_t height, nvrhi::ITexture *source);

    /// @brief Whether there are copies to draw into.
    [[nodiscard]] bool Ready() const { return _levelCount > 0u; }

    /// @brief The finished glow, half the screen's size, once Run has drawn it.
    [[nodiscard]] nvrhi::ITexture *Output() const;

    /// @brief Draws the chain down from the source and back up, each smaller
    /// copy blended over the larger by @p scatter. Ready() first.
    void Run(nvrhi::ICommandList *commandList, float scatter) const;

    void Shutdown();

private:
    /// @brief One copy and the two draws into it: down from the copy above,
    /// and back up from the one below.
    struct Level
    {
        nvrhi::TextureHandle texture;
        nvrhi::FramebufferHandle framebuffer;
        nvrhi::BindingSetHandle downBindings;
        nvrhi::BindingSetHandle upBindings;
        glm::uvec2 extent{0u};
    };

    void Draw(nvrhi::ICommandList *commandList, nvrhi::IGraphicsPipeline *pipeline, const Level &target,
              nvrhi::IBindingSet *bindings, const BloomConstants &constants) const;

    nvrhi::IDevice *_device = nullptr;
    nvrhi::ShaderHandle _downShader;
    nvrhi::ShaderHandle _upShader;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::GraphicsPipelineHandle _downPipeline;
    nvrhi::GraphicsPipelineHandle _upPipeline;
    std::array<Level, kMaxBloomLevels> _levels;
    glm::uvec2 _sourceExtent{0u};
    uint32_t _levelCount = 0u;
};

} // namespace Assisi::Render
