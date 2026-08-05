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

/// @brief The ambient term every surface gets for free, when nobody says otherwise.
///
/// Named here rather than left as a literal in three places because it is a value
/// with a history: it was `const float kAmbient = 0.03` inside cube_min.frag, and
/// moving it into the frame constants must not change a single rendered pixel by
/// accident. Anything that wants a different one passes it.
inline constexpr float kDefaultAmbientIntensity = 0.03f;

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
    ///
    /// @p ambientColor / @p ambientIntensity are the uniform term every surface
    /// receives regardless of what lights it. The defaults reproduce the constant
    /// this replaced (`kAmbient = 0.03` in cube_min.frag) exactly, so a caller that
    /// says nothing renders what it always did; the editor turns it up to inspect a
    /// model without having to light one.
    void UpdateFrameConstants(nvrhi::ICommandList *commandList, const glm::mat4 &viewProjection, const glm::mat4 &view,
                              uint32_t screenWidth, uint32_t screenHeight, float nearZ, float farZ,
                              uint32_t dirLightCount, MaterialDebugView debugView = MaterialDebugView::None,
                              const glm::vec3 &ambientColor = glm::vec3(1.f, 1.f, 1.f),
                              float             ambientIntensity = kDefaultAmbientIntensity) const;

    /// @brief Submission counts from one Submit — the consumer half of the
    /// draw-stats (the producer counts drawn/culled). Since stage E the pass builds
    /// a CPU-side indirect command buffer and multi-draws it, so these describe the
    /// batching the frame collapsed to: @p instances is the per-instance records
    /// uploaded (== DrawItems drawn); @p batches is the instanced draw commands
    /// after coalescing consecutive same-(mesh,submesh) items (the count that drops
    /// as the scene instances better — see the "Sort Draws" A/B); @p drawCalls is
    /// the `drawIndexedIndirect` API calls issued (one per arena buffer-group, so
    /// ~1 while everything shares the one GeometryArena from stage C).
    struct SubmitStats
    {
        uint32_t instances = 0;
        uint32_t batches   = 0;
        uint32_t drawCalls = 0;
    };

    /// @brief One per-object record: uploaded into the instance buffer each frame
    /// and indexed in the vertex shader by gl_InstanceIndex (each draw sets its
    /// startInstanceLocation). Mirrors cube_min.vert's `InstanceData` std430
    /// struct — mat4 (0..63) + uint (64), padded to the 16-byte array stride
    /// std430 gives the struct.
    ///
    /// Lives here rather than in the .cpp only so the pass can keep a reusable
    /// scratch vector of these across frames; it is not part of the caller-facing
    /// API and no other translation unit builds one.
    struct InstanceData
    {
        glm::mat4 model;
        uint32_t  materialIndex; // row into the material table (== Material::Id()).
        uint32_t  _pad0 = 0, _pad1 = 0, _pad2 = 0;
    };
    static_assert(sizeof(InstanceData) == 80, "InstanceData must match the shader's std430 array stride.");

    /// @brief Records `items` into `frame` as CPU-built indirect draws (stage E).
    /// Uploads all per-object data (world matrix + material id) into the instance
    /// buffer first — one contiguous record per item, indexed in the shader by
    /// gl_InstanceIndex — then coalesces consecutive items that share geometry
    /// (same mesh + submesh) into instanced `DrawIndexedIndirectArguments`: a run
    /// of N such items becomes one command with instanceCount = N and
    /// startInstanceLocation at the run's first record. Because the material id is
    /// per-instance (read from that record), a batch may mix materials — only the
    /// geometry range must match. The command buffer is multi-drawn with one
    /// `drawIndexedIndirect` per arena buffer-group (~1, since stage C shares one
    /// GeometryArena). The caller sorts the span by DrawItem::sortKey
    /// (material/mesh-major): that ordering places identical same-material meshes
    /// adjacent, so they coalesce — the sort now drives instancing, not binds.
    /// Items must reference live resources (valid until the frame is submitted).
    /// @param frame  The frame's command list + framebuffer + viewport size.
    /// @pre IsValid() — call Initialize() first, and UpdateFrameConstants() this frame.
    [[nodiscard]] SubmitStats Submit(const RenderFrame &frame, std::span<const DrawItem> items) const;

    /// @brief The GPU-built draw list a MeshCuller produced this frame (stage F1):
    /// the compute pass wrote the per-instance records + indirect commands + the
    /// draw count itself, so SubmitIndirect binds them and issues one
    /// drawIndexedIndirectCount rather than recording draws on the CPU.
    struct IndirectDrawInputs
    {
        /// Per-instance records (world matrix + material id), read by the vertex
        /// shader via gl_InstanceIndex (== each command's firstInstance). Bound at
        /// t6 in place of the CPU path's own instance buffer.
        nvrhi::IBuffer *instanceBuffer = nullptr;
        /// The batch draw commands (one DrawIndexedIndirectArguments per distinct
        /// (mesh, submesh)); the cull pass grew each one's instanceCount. Empty
        /// batches carry instanceCount 0 and draw nothing.
        nvrhi::IBuffer *indirectBuffer = nullptr;
        /// Number of batch commands to draw (CPU-known — every batch has a command,
        /// so this is the count for a plain drawIndexedIndirect; no count buffer).
        uint32_t commandCount = 0;
        /// The shared GeometryArena's vertex/index buffers every draw addresses.
        /// F1 assumes a single arena (as stage E does — one drawIndexedIndirect
        /// group); a second arena would need per-arena count buffers.
        nvrhi::IBuffer *vertexBuffer = nullptr;
        nvrhi::IBuffer *indexBuffer  = nullptr;
    };

    /// @brief Draws a GPU-culled frame: binds the global set against @p in's
    /// instance buffer, points the pipeline at the cull pass's batch-command
    /// buffer, and issues one `drawIndexedIndirect` over @p in.commandCount
    /// commands (empty batches draw 0 instances). Reports the API call in
    /// `drawCalls`; the caller reports the survivor/batch tallies from the culler.
    /// @pre IsValid(), UpdateFrameConstants() this frame, and a MeshCuller::Cull
    ///      recorded into the same command list ahead of this call.
    [[nodiscard]] SubmitStats SubmitIndirect(const RenderFrame &frame, const IndirectDrawInputs &in) const;

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
    /// table, and the per-instance buffer at t6 — @p instanceBuffer, which is the
    /// pass's own (CPU path) or the MeshCuller's (GPU path). Rebuilt when that
    /// buffer handle changes (a growth, or a switch between paths) or after
    /// InvalidateBindingSets().
    nvrhi::IBindingSet *GetOrCreateGlobalBindingSet(nvrhi::IBuffer *instanceBuffer) const;

    /// @brief Ensures the indirect-args buffer holds at least @p commandCount
    /// DrawIndexedIndirectArguments, reallocating with geometric growth when not.
    /// Unlike the instance buffer this handle is never referenced by a binding set
    /// (it binds via GraphicsState::indirectParams), so a growth needs no set rebuild.
    void EnsureIndirectCapacity(uint32_t commandCount) const;

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

    // CPU-built indirect draw-command buffer (stage E): one
    // DrawIndexedIndirectArguments per instanced batch, rebuilt and multi-drawn
    // each frame. Grown geometrically like the instance buffer; not a plain
    // Render::Buffer since it is an indirect-args buffer (isDrawIndirectArgs), not
    // a structured SRV. Owned by the pass.
    mutable nvrhi::BufferHandle          _indirectBuffer;
    mutable uint32_t                     _indirectCapacity = 0; // in commands

    // Per-frame scratch for Submit, kept across frames so the steady state costs
    // no allocations: clear() preserves capacity, whereas the locals these replaced
    // heap-allocated (and grew) on every single frame.
    mutable std::vector<InstanceData>                        _scratchInstances;
    mutable std::vector<nvrhi::DrawIndexedIndirectArguments> _scratchCommands;
    mutable std::vector<const MeshBuffer *>                  _scratchBatchMeshes;
};
} /* namespace Assisi::Render */
