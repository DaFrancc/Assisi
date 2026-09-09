/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyPass.hpp
/// @brief Fills the pixels the scene left empty with sky.

#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/Sky.hpp>
#include <Assisi/Render/Texture.hpp>

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
    /// @brief Whether the moon's albedo photograph has been read yet, and how it
    /// went.
    ///
    /// Reported because the failure is invisible: a failed load leaves a flat
    /// white disk, which is a perfectly plausible moon and is exactly what the
    /// disk looked like before there was a texture at all.
    enum class MoonTexture : std::uint8_t
    {
        /// Nothing has asked for a moon yet, so nothing has been opened. A level
        /// with no Moon component stays here for ever and pays nothing.
        NotLoaded,
        Loaded,
        Failed,
    };

    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// Format/sample-count of the scene target the sky composites into,
        /// including its depth attachment.
        nvrhi::FramebufferInfo framebufferInfo;
        std::string vertexShaderSpvPath;
        std::string pixelShaderSpvPath;
        /// The moon's albedo, opened on the first frame a moon is actually drawn
        /// rather than here. A level without a moon never opens it.
        std::string moonTexturePath;
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
              const SkySun &sun, const SkyMoon &moon, const SkySettings &settings);

    [[nodiscard]] MoonTexture MoonTextureState() const { return _moonState; }

private:
    [[nodiscard]] bool BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);
    [[nodiscard]] bool BuildBindingSet();

    /// @brief Read the moon's albedo, once, on the first frame a moon is drawn.
    ///
    /// **sRGB, not linear.** The file is an albedo multiplied into a radiance, so
    /// it loads the way base colour and emissive do and the GPU decodes it to
    /// linear at sample time. Loading it linear makes every texel a quarter to a
    /// half too dark, and the tempting "fix" — raising the disk intensity until
    /// it looks right — buries the mistake where nobody will find it.
    void LoadMoonTexture();

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::BufferHandle _constantsBuffer;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::BindingSetHandle _bindingSet;
    nvrhi::GraphicsPipelineHandle _pipeline;

    std::string _moonTexturePath;
    /// A white 1x1 until a moon is first drawn, so a binding set always has a
    /// real texture in it and the pipeline never has a variant. A failed load
    /// leaves this in place, which is the flat tinted disk — degraded, not broken.
    Texture _moon;
    nvrhi::SamplerHandle _moonSampler;
    MoonTexture _moonState = MoonTexture::NotLoaded;
};

} // namespace Assisi::Render
