/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowPass.hpp
/// @brief The sun's cascade array and the depth-only pipeline that fills it.
///
/// One texture array, one slice per cascade, one framebuffer each. The pipeline
/// is vertex-only — there is no fragment stage, because nothing but depth is
/// written — and the draws are instanced indirect commands built the same way
/// MeshPass builds them, over the same GeometryArena.
///
/// Nothing here is allocated until a shadow-casting sun exists. Configure(...,
/// active = false) drops the array, the framebuffers and the pipeline, leaving
/// a one-texel placeholder so the mesh pass's binding set still has something
/// to point at. A scene with no sun therefore pays a single texel and no pass.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/ShadowCascades.hpp>
#include <Assisi/Render/ShadowSettings.hpp>

namespace Assisi::Render
{

class ShadowPass
{
public:
    ShadowPass() = default;

    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// Compiled-SPIR-V path of the depth-only vertex stage.
        std::string vertexShaderSpvPath;
    };

    /// @brief Loads the vertex stage and creates the placeholder cascade
    /// texture. The pipeline and the real array wait for Configure().
    /// @return false if the shader failed to load or the placeholder failed to
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
    [[nodiscard]] bool Configure(const ShadowSettings &settings, bool active);

    /// @brief One shadow-casting submesh instance.
    ///
    /// The world sphere is carried rather than recomputed because the caller
    /// already has it from the mesh's local bounds and the instance's matrix,
    /// and every cascade tests against it.
    struct Caster
    {
        const MeshBuffer *mesh = nullptr;
        std::uint32_t submeshIndex = 0;
        glm::mat4 model{1.f};
        Geometry::BoundingSphere worldSphere;
    };

    /// @brief What one Render() drew, per cascade summed.
    struct Stats
    {
        std::uint32_t cascades = 0;  ///< Cascades rendered (0 when inactive).
        std::uint32_t instances = 0; ///< Caster instances submitted, counted once per cascade they survive into.
        std::uint32_t batches = 0;   ///< Instanced draw commands after coalescing same-geometry runs.
        std::uint32_t drawCalls = 0; ///< drawIndexedIndirect calls issued — one per cascade with anything in it.
        std::uint32_t culled = 0;    ///< Caster-cascade pairs the per-cascade frustum test rejected.
    };

    /// @brief Clear every cascade and draw @p casters into them.
    ///
    /// @p casters must be sorted by (mesh, submesh) — consecutive items with
    /// the same geometry coalesce into one instanced draw, exactly as in
    /// MeshPass::Submit, and an unsorted span merely draws more commands.
    ///
    /// Each cascade culls the span against its own frustum. The cascade
    /// matrices already reach back to the casters (see CascadeFitParams), so a
    /// caster behind the camera survives the test rather than being clipped.
    Stats Render(nvrhi::ICommandList *commandList, const CascadeFit &fit, std::span<const Caster> casters) const;

    [[nodiscard]] bool IsActive() const { return _active && _pipeline != nullptr; }

    /// @brief The cascade array the mesh shader samples. Never null after a
    /// successful Initialize() — it is the one-texel placeholder while the pass
    /// is inactive, so the mesh pass's binding set never has a hole in it.
    [[nodiscard]] nvrhi::ITexture *CascadeTexture() const { return _cascadeTexture; }

    /// @brief The settings the current allocation was built for.
    [[nodiscard]] const ShadowSettings &Settings() const { return _settings; }

private:
    /// @brief One per-object record. A bare world matrix: the depth pass has no
    /// material, no normal and no texture coordinate to carry.
    struct InstanceData
    {
        glm::mat4 model;
    };
    static_assert(sizeof(InstanceData) == 64, "InstanceData must match the shader's std430 array stride.");

    [[nodiscard]] bool RebuildTargets();
    [[nodiscard]] bool RebuildPipeline();
    void ReleaseTargets();
    /// @brief The one-texel array bound while the pass is inactive.
    [[nodiscard]] bool CreatePlaceholder();

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::InputLayoutHandle _inputLayout;
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::GraphicsPipelineHandle _pipeline;

    // The cascade array, and one framebuffer per slice. Empty while inactive.
    nvrhi::TextureHandle _cascadeTexture;
    std::vector<nvrhi::FramebufferHandle> _cascadeFramebuffers;
    // Bound while inactive so the mesh pass always has a texture to sample.
    nvrhi::TextureHandle _placeholderTexture;

    ShadowSettings _settings;
    bool _active = false;
    // What the current allocation was built for, so Configure can tell an edit
    // that needs a reallocation from one that only needs a pipeline rebuild.
    std::uint32_t _builtCascades = 0;
    std::uint32_t _builtResolution = 0;
    ShadowMapFormat _builtFormat = ShadowMapFormat::D32;
    float _builtSlopeBias = -1.f;
    bool _builtCullFrontFaces = false;

    // Per-instance world matrices for every cascade in one buffer, rebuilt each
    // frame; grown geometrically, which swaps the handle and so invalidates the
    // cached binding set (GetOrCreateBindingSet notices).
    mutable Buffer _instanceBuffer;
    mutable nvrhi::BindingSetHandle _bindingSet;
    mutable const nvrhi::IBuffer *_bindingSetInstanceBuffer = nullptr;

    mutable nvrhi::BufferHandle _indirectBuffer;
    mutable std::uint32_t _indirectCapacity = 0; // in commands

    // Per-frame scratch, kept across frames so a steady state allocates nothing.
    mutable std::vector<InstanceData> _scratchInstances;
    mutable std::vector<nvrhi::DrawIndexedIndirectArguments> _scratchCommands;
    mutable std::vector<const MeshBuffer *> _scratchBatchMeshes;
    /// Where each cascade's run of commands starts in _scratchCommands.
    mutable std::array<std::uint32_t, kMaxShadowCascades + 1> _scratchCascadeCommandStart{};

    [[nodiscard]] nvrhi::IBindingSet *GetOrCreateBindingSet(nvrhi::IBuffer *instanceBuffer) const;
    void EnsureIndirectCapacity(std::uint32_t commandCount) const;
};

} // namespace Assisi::Render
