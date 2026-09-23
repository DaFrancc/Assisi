/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowPass.hpp
/// @brief The sun's cascade array, and the strategy that fills it.
///
/// Two texture arrays, one slice per cascade in each, one framebuffer each: the
/// still casters' depth, and the slice the shader reads, which is that with the
/// moving casters drawn over it (see Render). The drawing
/// itself belongs to ShadowDepthRenderer, which knows nothing about cascades —
/// this owns what is specific to the sun: how many slices there are, what
/// format they take, when they are cleared, and the pipeline state their depth
/// format and cull side imply.
///
/// Nothing here is allocated until a shadow-casting sun exists. Configure(...,
/// active = false) drops the array, the framebuffers and the pipeline, leaving
/// a one-texel empty array so the mesh pass's binding set still has something to
/// point at. A scene with no sun therefore pays a single texel and no pass.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ShadowCadence.hpp>
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
        /// A triangle covering the viewport, and the fragment stage that writes
        /// a cascade's still depth through it: what puts the still depth back
        /// under where movers stood.
        std::string restoreVertexShaderSpvPath;
        std::string restorePixelShaderSpvPath;
    };

    /// @brief Bind to the device and the shared renderer, and create the empty
    /// cascade texture. The pipeline and the real array wait for Configure().
    /// @return false if the renderer is unusable or the empty texture failed to
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
        /// Cascades that kept the depth they already held. On a still scene with
        /// a fixed sun this is every cascade and @ref cascades is zero, which is
        /// the reading the pay-for-what-you-place gate is taken from.
        std::uint32_t cascadesKept = 0;
        std::uint32_t instances = 0; ///< Caster instances submitted, counted once per cascade they survive into.
        std::uint32_t batches = 0;   ///< Instanced draw commands after coalescing same-geometry runs.
        /// How many of @ref batches drew through the alpha-testing pipeline.
        /// Zero for a scene whose casters are all opaque, which is what makes
        /// "the cutouts cost nothing here" a reading rather than a claim.
        std::uint32_t maskedBatches = 0;
        std::uint32_t drawCalls = 0; ///< drawIndexedIndirect calls issued — one per cascade with anything in it.
        std::uint32_t culled = 0;    ///< Caster-cascade pairs no cascade drew, classification and frustum together.

        /// Casters each cascade drew, nearest first. The split @ref instances
        /// sums away: a near cascade covering a courtyard and a far one covering
        /// the district cost very differently, and only the per-cascade figure
        /// says which of them a rise in the total came from.
        std::array<std::uint32_t, kMaxShadowCascades> cascadeCasters{};

        /// Cascades whose movers were drawn again over their still depth.
        std::uint32_t moverCascades = 0;
    };

    /// @brief Bring the cascades up to date with @p plan: rebake the still
    /// layers it names, and redraw the movers where it says they changed.
    ///
    /// Each cascade is two slices. The still layer holds the still casters'
    /// depth and nothing else; the slice the shader reads is that depth with
    /// the moving casters drawn over it. A rebaked still layer is written whole
    /// into the read slice; otherwise, where the movers last stood is written
    /// back from the still layer before they are drawn where they stand now.
    ///
    /// @p casters must be sorted by ShadowGeometryKey — consecutive items with
    /// the same key coalesce into one instanced draw, and an unsorted span
    /// merely draws more commands.
    ///
    /// A caster's view mask is **indexed by position** in the plan's lists
    /// rather than by cascade: bit i names @p plan.redraw[i] (a still caster
    /// for that still layer), and bit redrawCount + j names
    /// @p plan.movingRedraw[j] (a moving caster for that read slice). A cascade
    /// neither list names keeps both slices as they are, which is only correct
    /// while @p plan.fit carries the matrix they were rasterized with.
    Stats Render(nvrhi::ICommandList *commandList, const SunShadowCadencePlan &plan,
                 std::span<const ShadowCaster> casters) const;

    [[nodiscard]] bool IsActive() const { return _active && _pipelines[static_cast<std::uint32_t>(MeshPipeline::Opaque)] != nullptr; }

    /// @brief Count the casters each cascade drew, for a diagnostic that is
    /// showing them.
    ///
    /// Off by default, and the default is the point: nothing in the picture
    /// depends on the answer, so a frame with no panel open must not walk a
    /// cascade's commands to produce it. Cheap is not the same as free.
    void SetCascadeCountsEnabled(bool enabled) { _cascadeCounts = enabled; }

    /// @brief The cascade array the mesh shader samples. Never null after a
    /// successful Initialize() — it is the one-texel empty array while the pass
    /// is inactive, so the mesh pass's binding set never has a hole in it.
    [[nodiscard]] nvrhi::ITexture *CascadeTexture() const { return _cascadeTexture; }

    /// @brief Index of this pass's first cascade in the frame's view table.
    [[nodiscard]] std::uint32_t FirstView() const { return _firstView; }

    /// @brief The settings the current allocation was built for.
    [[nodiscard]] const SunShadowSettings &Settings() const { return _settings; }

    /// @brief Counts allocations of the cascade array.
    ///
    /// A change means the slices hold depth of a texture that no longer exists,
    /// or no depth at all. Anything keeping a cascade across frames has to
    /// notice that, and comparing settings for it would mean a second copy of
    /// the rule about which of them force a reallocation.
    [[nodiscard]] std::uint32_t AllocationGeneration() const { return _allocationGeneration; }

private:
    [[nodiscard]] bool RebuildTargets();
    [[nodiscard]] bool RebuildPipeline();
    /// @brief The handles as the renderer wants them, one per pipeline class.
    [[nodiscard]] ShadowPipelines PipelineSet() const;
    void ReleaseTargets();
    /// @brief Create the one-texel array bound while the pass is inactive.
    [[nodiscard]] bool CreateNoCascadesTexture();
    /// @brief Write back from the still layer every tile of @p cascade's read
    /// slice a mover was last drawn into, and forget them.
    void RestoreMoverTiles(nvrhi::ICommandList *commandList, std::uint32_t cascade) const;
    /// @brief Write @p cascade's still depth over @p region of its read slice.
    void RestoreStillDepth(nvrhi::ICommandList *commandList, std::uint32_t cascade, const nvrhi::Rect &region) const;
    /// @brief Note the tiles this frame's movers were drawn into, for each
    /// cascade @p movingRedraw names.
    void RecordMoverTiles(const CascadeFit &fit, std::span<const std::uint32_t> movingRedraw) const;

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
    // The still layers, one slice per cascade: the still casters' depth alone,
    // which the read slices above are put back from. Empty while inactive.
    nvrhi::TextureHandle _stillTexture;
    std::vector<nvrhi::FramebufferHandle> _stillFramebuffers;
    // What writes the still depth back into a read slice (RestoreStillDepth).
    // A draw rather than a copy: a copy moves both arrays to transfer layouts,
    // which on a GPU that compresses depth costs a pass over the whole array
    // every frame anything moves.
    nvrhi::ShaderHandle _restoreVertexShader;
    nvrhi::ShaderHandle _restorePixelShader;
    nvrhi::BindingLayoutHandle _restoreLayout;
    nvrhi::BindingSetHandle _restoreSet;
    nvrhi::GraphicsPipelineHandle _restorePipeline;
    // Bound while the pass is inactive, so the mesh pass always has a texture to
    // sample. Permanent, not scaffolding: a scene with no sun never leaves it.
    nvrhi::TextureHandle _noCascadesTexture;

    SunShadowSettings _settings;
    bool _active = false;
    // Whether Stats::cascadeCasters is filled. See SetCascadeCountsEnabled.
    bool _cascadeCounts = false;
    // What the current allocation was built for, so Configure can tell an edit
    // that needs a reallocation from one that only needs a pipeline rebuild.
    std::uint32_t _builtCascades = 0;
    std::uint32_t _builtResolution = 0;
    // Bumped by every allocation and every release. See AllocationGeneration.
    std::uint32_t _allocationGeneration = 0;
    ShadowMapFormat _builtFormat = ShadowMapFormat::D32;
    float _builtSlopeBias = -1.f;

    /// Where this frame's cascades start in the shared view table.
    mutable std::uint32_t _firstView = 0;
    // Per-frame scratch, kept across frames so a steady state allocates nothing.
    mutable std::vector<ShadowDepthTarget> _scratchTargets;
    mutable std::vector<ShadowCaster> _stillCasters;
    mutable std::vector<ShadowCaster> _movingCasters;
    // Per cascade, which tiles of its read slice movers were last drawn into, a
    // byte per tile row-major: what has to be put back before they are drawn
    // again. Cleared by a whole-slice copy.
    mutable std::array<std::vector<std::uint8_t>, kMaxShadowCascades> _moverTiles;
};

} // namespace Assisi::Render
