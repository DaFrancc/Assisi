/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshPass.hpp
/// @brief NVRHI graphics pipeline for drawing lit, opaque scene geometry.
///
/// One pipeline shared by every entity, and — since stage D — one binding set
/// too: `MeshBuffer`'s vertex layout in, per-object data (world matrix + material
/// id) read from a per-instance structured buffer indexed by gl_InstanceIndex, a
/// per-frame constant buffer carrying the camera / view-projection / cluster-grid
/// parameters, the material's glTF metallic-roughness factors fetched from the
/// AssetCache's material table (and its textures from the bindless table) by that
/// id, and clustered point/spot/directional lighting (see ClusterGrid) read
/// directly from cube_min.frag. Nothing binds per-draw any more — the whole span
/// draws against one binding set + the bindless table, one drawIndexed per item
/// with a distinct startInstanceLocation. That is the precondition for stage E's
/// indirect/instanced draws.

#include <cstdint>
#include <span>
#include <string>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/ClusterGrid.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/Material.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/RenderFrame.hpp>

namespace Assisi::Render
{
/// @brief Which material channel the mesh pass visualizes instead of the lit
/// result. None = the normal render; the rest short-circuit the shader to one
/// channel for debugging. Values must match the `kDebug*` constants in
/// cube_min.frag (packed into FrameConstants).
enum class MaterialDebugView : uint32_t
{
    None = 0,
    BaseColor,
    Metallic,
    Roughness,
    Normal,
    Occlusion,
    Emissive,
};

class MeshPass
{
  public:
    MeshPass() = default;

    /// @brief Inputs to Initialize(). Grouped into a struct so the call site
    /// stays legible and adding an input doesn't ripple through every caller.
    struct InitParams
    {
        nvrhi::IDevice        *device = nullptr;
        nvrhi::FramebufferInfo framebufferInfo;
        /// Compiled-SPIR-V shader paths (see ShaderModule.hpp).
        std::string vertexShaderSpvPath;
        std::string pixelShaderSpvPath;
        /// Must outlive the pass — its light buffers bind into every binding set.
        const ClusterGrid *clusterGrid = nullptr;
        /// The AssetCache's bindless material-texture table + layout (stage D):
        /// the layout joins the pipeline as register space 1, the table binds
        /// every draw. Both must outlive the pass (AssetCache owns them; the
        /// handles stay stable across Clear()).
        nvrhi::IBindingLayout   *bindlessLayout = nullptr;
        nvrhi::IDescriptorTable *bindlessTable = nullptr;
        /// The AssetCache's material table (stage D): a structured buffer whose
        /// row `Material::Id()` holds that material's constants. Bound into the
        /// pass's one binding set; the shader fetches a row per instance. Fixed
        /// capacity, so the handle is stable across Clear(). Must outlive the pass.
        nvrhi::IBuffer *materialTable = nullptr;
    };

    /// @brief Loads the shaders and builds the pipeline against the framebuffer
    /// format in @p params.
    /// @return false if either shader failed to load or the pipeline failed to build.
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Recreates just the graphics pipeline against a new FramebufferInfo,
    /// reusing the shaders/input layout/binding layout already loaded by
    /// Initialize(). Needed when the render target's sample count changes at
    /// runtime (see Render::PostProcess) — a pipeline built for sampleCount=1
    /// isn't compatible with an MSAA framebuffer. Existing binding sets (which
    /// don't depend on FramebufferInfo) stay valid and cached.
    /// @pre IsValid() — call Initialize() first.
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Updates the per-frame constant buffer (view-projection + camera view
    /// matrix + cluster-grid parameters). Call once per frame, before Submit. The
    /// vertex shader multiplies @p viewProjection by each instance's world matrix
    /// for clip position, so the model matrix never leaves the GPU.
    void UpdateFrameConstants(nvrhi::ICommandList *commandList, const glm::mat4 &viewProjection, const glm::mat4 &view,
                              uint32_t screenWidth, uint32_t screenHeight, float nearZ, float farZ,
                              uint32_t dirLightCount, MaterialDebugView debugView = MaterialDebugView::None) const;

    /// @brief State-change counts from one Submit — the consumer half of the
    /// draw-stats (the producer counts drawn/culled). Since stage D the pass binds
    /// one global set, so these are no longer bind counts but batching diagnostics:
    /// materialBinds is the distinct material-id runs the sort produced (a proxy
    /// for how instanceable the frame is), meshBinds the distinct vertex-buffer
    /// runs — ~1, since the shared GeometryArena (stage C) puts every mesh in one
    /// buffer.
    struct SubmitStats
    {
        uint32_t drawCalls    = 0;
        uint32_t materialBinds = 0;
        uint32_t meshBinds     = 0;
    };

    /// @brief Records `items` into `frame` in order — one drawIndexed per DrawItem.
    /// Uploads all per-object data (world matrix + material id) into the instance
    /// buffer first, then draws each item with a distinct startInstanceLocation so
    /// the vertex shader reads its record via gl_InstanceIndex; the world matrix
    /// and material id never leave the GPU. The caller sorts the span by
    /// DrawItem::sortKey (material/mesh-major) for early-Z and future instancing;
    /// with one binding set the sort no longer affects bind counts. Items must
    /// reference live resources (valid until the frame is submitted).
    /// @param frame  The frame's command list + framebuffer + viewport size.
    /// @pre IsValid() — call Initialize() first, and UpdateFrameConstants() this frame.
    [[nodiscard]] SubmitStats Submit(const RenderFrame &frame, std::span<const DrawItem> items) const;

    bool IsValid() const { return _pipeline != nullptr; }

    /// @brief Drops the cached global binding set so the next Submit rebuilds it.
    /// Kept for the level-unload path (SceneRenderer::InvalidateAssetBindings): the
    /// set references only handles that survive an AssetCache::Clear (frame CB,
    /// sampler, light buffers, material table, instance buffer), so this is really
    /// just hygiene now — the rebuild produces an identical set — but it costs
    /// nothing and keeps the seam if a referenced resource is ever recreated.
    void InvalidateBindingSets() { _globalBindingSet = nullptr; }

  private:
    /// @brief Builds (once, then caches) the pass's single binding set: frame
    /// constants, the shared sampler, the clustered-light buffers, the material
    /// table, and the per-instance buffer. Rebuilt when the instance buffer grows
    /// (its handle changes) or after InvalidateBindingSets().
    nvrhi::IBindingSet *GetOrCreateGlobalBindingSet() const;

    nvrhi::IDevice *_device = nullptr;
    const ClusterGrid *_clusterGrid = nullptr;

    nvrhi::ShaderHandle           _vertexShader;
    nvrhi::ShaderHandle           _pixelShader;
    nvrhi::InputLayoutHandle      _inputLayout;
    nvrhi::BindingLayoutHandle    _bindingLayout;
    nvrhi::SamplerHandle          _sampler;
    nvrhi::GraphicsPipelineHandle _pipeline;
    nvrhi::BufferHandle           _frameConstantsBuffer;

    // The AssetCache's bindless material-texture table + its layout, and its
    // material table (stage D). Non-owning: the AssetCache owns them and keeps the
    // handles stable across Clear(). The bindless layout is register space 1 in
    // the pipeline; the table is bound as the second binding set each draw. The
    // material table binds into the global binding set (set 0).
    nvrhi::BindingLayoutHandle   _bindlessLayout;
    nvrhi::DescriptorTableHandle _bindlessTable;
    nvrhi::IBuffer              *_materialTable = nullptr;

    // Per-instance data (world matrix + material id), rebuilt and uploaded each
    // frame from the sorted DrawItems; the vertex shader indexes it by
    // gl_InstanceIndex (= each draw's startInstanceLocation). Grown geometrically
    // when a frame has more items than it holds — a growth swaps the buffer handle,
    // which invalidates _globalBindingSet (see GetOrCreateGlobalBindingSet). Owned
    // by the pass, unlike the AssetCache-owned tables above.
    mutable Buffer                       _instanceBuffer;
    mutable nvrhi::BindingSetHandle      _globalBindingSet;
    // The instance-buffer handle _globalBindingSet was built against; a mismatch
    // means the buffer grew and the set must be rebuilt.
    mutable const nvrhi::IBuffer        *_globalSetInstanceBuffer = nullptr;
};
} /* namespace Assisi::Render */
