/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyPass.hpp
/// @brief Fills the pixels the scene left empty with sky.

#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Fog.hpp>
#include <Assisi/Render/FogVolumeBuffers.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/Sky.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Render
{

/// @brief What the sky pass needs to draw placed fog volumes in front of the
/// sky. Mirrors sky.frag's SkyFogVolumes block.
struct SkyFogConstants
{
    /// The level's fog, whose haze colours a lit volume and covers the stretch
    /// of sky between a volume and the eye.
    FogConstants fog;
    /// FogVolumeFrame::Lanes. Zero in a capture, which draws none.
    glm::uvec4 volumes{0u};
};

ASSISI_GPU_LAYOUT(SkyFogConstants);
ASSISI_GPU_FIRST_FIELD(SkyFogConstants, fog);
ASSISI_GPU_FIELD_AFTER(SkyFogConstants, volumes, fog);
ASSISI_GPU_NO_TAIL_PADDING(SkyFogConstants, volumes);

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
        /// The placed fog volumes. Must outlive the pass — its buffers bind into
        /// the pass's binding set.
        const FogVolumeBuffers *fogVolumes = nullptr;
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

    /// @brief What the next Draw does for the level's fog. @p color is read only
    /// by SkyFogMode::Flat, where it is the whole of what is drawn. A capture
    /// through DrawInto never takes it.
    void SetFog(SkyFogMode mode, const glm::vec3 &color)
    {
        _fog = glm::vec4(color, static_cast<float>(mode));
    }

    /// @brief The placed fog volumes the next Draw shows in front of the sky,
    /// and the level's fog they are seen through. A capture through DrawInto
    /// never takes them.
    void SetFogVolumes(const FogConstants &fog, const glm::uvec4 &lanes)
    {
        _fogVolumes = SkyFogConstants{.fog = fog, .volumes = lanes};
    }

    /// @brief The clouds' sky map every later Draw and DrawInto composites over
    /// the sky (CloudPass owns it), or null for a sky with none. A capture
    /// takes the clouds too, so reflections show them.
    void SetClouds(nvrhi::ITexture *cloudMap);

    /// @brief Fill every texel of @p target with the sky @p constants describe.
    ///
    /// How a reflection probe captures the sky: the same shader the screen is
    /// drawn with, so a reflection and the sky behind it cannot disagree about
    /// what the sky looks like. No depth test — a capture target is all sky —
    /// and no profiler scope, because the caller's scope is the one that names
    /// what the capture is for.
    ///
    /// The moon's texture is not loaded here. A capture that wants the moon's
    /// disk has to draw a frame that shows it first; the probe leaves the disks
    /// out, so it never does.
    ///
    /// The pipeline is built on the first call, against @p target's format, so
    /// a level with no probe never builds it. Every later target must share
    /// that format.
    void DrawInto(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *target, const SkyConstants &constants);

    [[nodiscard]] MoonTexture MoonTextureState() const { return _moonState; }

private:
    [[nodiscard]] bool BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);
    [[nodiscard]] bool BuildBindingSet();

    /// @brief Upload @p constants and draw the fullscreen triangle into
    /// @p target through @p pipeline.
    void Submit(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *target, nvrhi::IGraphicsPipeline *pipeline,
                const nvrhi::Viewport &viewport, const SkyConstants &constants, const SkyFogConstants &fog);

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
    nvrhi::BufferHandle _fogConstantsBuffer;
    const FogVolumeBuffers *_fogVolumeBuffers = nullptr;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::BindingSetHandle _bindingSet;
    nvrhi::GraphicsPipelineHandle _pipeline;
    // DrawInto's: colour only, no depth test. Independent of the scene target,
    // so a render-target change leaves it alone.
    nvrhi::GraphicsPipelineHandle _capturePipeline;

    /// SkyConstants::fog for the next Draw.
    glm::vec4 _fog{0.0f};

    /// The clouds' sky map, or null. The binding set names it, so a change of
    /// map rebuilds the set.
    nvrhi::ITexture *_cloudMap = nullptr;
    /// Bound in the map's place while there are no clouds. Never sampled: the
    /// shader reads the map only while SkyConstants says there are clouds.
    Texture _noClouds;
    nvrhi::SamplerHandle _cloudMapSampler;
    /// The placed fog volumes for the next Draw.
    SkyFogConstants _fogVolumes;

    std::string _moonTexturePath;
    /// A white 1x1 until a moon is first drawn, so a binding set always has a
    /// real texture in it and the pipeline never has a variant. A failed load
    /// leaves this in place, which is the flat tinted disk — degraded, not broken.
    Texture _moon;
    nvrhi::SamplerHandle _moonSampler;
    MoonTexture _moonState = MoonTexture::NotLoaded;
};

} // namespace Assisi::Render
