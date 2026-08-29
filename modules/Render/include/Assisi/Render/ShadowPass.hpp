/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowPass.hpp
/// @brief The sun's cascade array, and the strategy that fills it.
///
/// One texture array, one slice per cascade, one framebuffer each. The drawing
/// itself belongs to ShadowDepthRenderer, which knows nothing about cascades —
/// this owns what is specific to the sun: how many slices there are, what
/// format they take, when they are cleared, and the pipeline state their depth
/// format and cull side imply.
///
/// Nothing here is allocated until a shadow-casting sun exists. Configure(...,
/// active = false) drops the array, the framebuffers and the pipeline, leaving
/// a one-texel placeholder so the mesh pass's binding set still has something
/// to point at. A scene with no sun therefore pays a single texel and no pass.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowDepthRenderer.hpp>
#include <Assisi/Render/ShadowSettings.hpp>
#include <Assisi/Render/ShadowView.hpp>

namespace Assisi::Render
{

class ShadowPass
{
public:
    ShadowPass() = default;

    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// The shared depth renderer this pass draws through. Not owned, and it
        /// must outlive the pass.
        const ShadowDepthRenderer *depthRenderer = nullptr;
    };

    /// @brief Bind to the device and the shared renderer, and create the
    /// placeholder cascade texture. The pipeline and the real array wait for
    /// Configure().
    /// @return false if the renderer is unusable or the placeholder failed to
    /// allocate — either leaves the pass permanently inactive rather than
    /// failing the renderer.
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Bring the pass in line with @p settings.
    ///
    /// @p active is whether anything wants shadows this frame — settings
    /// enabled *and* a shadow-casting sun in the scene. False releases the
    /// array, the framebuffers and the pipeline.
    ///
    /// Cheap to call every frame: it compares against what is already built and
    /// returns immediately when nothing that affects an allocation changed.
    /// @return false if a rebuild was needed and failed; the pass goes inactive.
    [[nodiscard]] bool Configure(const SunShadowSettings &settings, bool active);

    /// @brief What one Render() drew, per cascade summed.
    struct Stats
    {
        std::uint32_t cascades = 0;  ///< Cascades rendered (0 when inactive).
        std::uint32_t instances = 0; ///< Caster instances submitted, counted once per cascade they survive into.
        std::uint32_t batches = 0;   ///< Instanced draw commands after coalescing same-geometry runs.
        /// How many of @ref batches drew through the alpha-testing pipeline.
        /// Zero for a scene whose casters are all opaque, which is what makes
        /// "the cutouts cost nothing here" a reading rather than a claim.
        std::uint32_t maskedBatches = 0;
        std::uint32_t drawCalls = 0; ///< drawIndexedIndirect calls issued — one per cascade with anything in it.
        std::uint32_t culled = 0;    ///< Caster-cascade pairs the per-cascade frustum test rejected.
    };

    /// @brief Clear every cascade and draw @p casters into them.
    ///
    /// @p casters must be sorted by ShadowGeometryKey — consecutive items with
    /// the same key coalesce into one instanced draw, and an unsorted span
    /// merely draws more commands.
    ///
    /// Each cascade culls the span against its own frustum. The cascade
    /// matrices already reach back to the casters (see CascadeFitParams), so a
    /// caster behind the camera survives the test rather than being clipped.
    Stats Render(nvrhi::ICommandList *commandList, const CascadeFit &fit, std::span<const ShadowCaster> casters) const;

    [[nodiscard]] bool IsActive() const { return _active && _pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] != nullptr; }

    /// @brief The cascade array the mesh shader samples. Never null after a
    /// successful Initialize() — it is the one-texel placeholder while the pass
    /// is inactive, so the mesh pass's binding set never has a hole in it.
    [[nodiscard]] nvrhi::ITexture *CascadeTexture() const { return _cascadeTexture; }

    /// @brief Index of this pass's first cascade in the frame's view table.
    [[nodiscard]] std::uint32_t FirstView() const { return _firstView; }

    /// @brief The settings the current allocation was built for.
    [[nodiscard]] const SunShadowSettings &Settings() const { return _settings; }

private:
    [[nodiscard]] bool RebuildTargets();
    [[nodiscard]] bool RebuildPipeline();
    /// @brief The handles as the renderer wants them, one per pipeline class.
    [[nodiscard]] ShadowPipelines PipelineSet() const;
    void ReleaseTargets();
    /// @brief The one-texel array bound while the pass is inactive.
    [[nodiscard]] bool CreatePlaceholder();

    nvrhi::IDevice *_device = nullptr;
    const ShadowDepthRenderer *_depthRenderer = nullptr;

    // One per MeshPipeline class: the alpha test and the cull mode are both
    // pipeline state and vary independently, so a caster's material decides
    // which of the four it is drawn through. Built from the same settings and
    // released together. A masked entry is null when the renderer has no
    // alpha-testing variant to build it from, which leaves cutouts casting a
    // solid silhouette rather than nothing.
    std::array<nvrhi::GraphicsPipelineHandle, kMeshPipelineCount> _pipelines;

    // The cascade array, and one framebuffer per slice. Empty while inactive.
    nvrhi::TextureHandle _cascadeTexture;
    std::vector<nvrhi::FramebufferHandle> _cascadeFramebuffers;
    // Bound while inactive so the mesh pass always has a texture to sample.
    nvrhi::TextureHandle _placeholderTexture;

    SunShadowSettings _settings;
    bool _active = false;
    // What the current allocation was built for, so Configure can tell an edit
    // that needs a reallocation from one that only needs a pipeline rebuild.
    std::uint32_t _builtCascades = 0;
    std::uint32_t _builtResolution = 0;
    ShadowMapFormat _builtFormat = ShadowMapFormat::D32;
    float _builtSlopeBias = -1.f;

    /// Where this frame's cascades start in the shared view table.
    mutable std::uint32_t _firstView = 0;
    // Per-frame scratch, kept across frames so a steady state allocates nothing.
    mutable std::vector<ShadowDepthTarget> _scratchTargets;
};

} // namespace Assisi::Render
