/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ShadowDepthRenderer.hpp
/// @brief Scene depth, from any view, into any rectangle of any target.
///
/// The whole of a shadow map's drawing: cull the casters against a view,
/// coalesce consecutive instances of the same geometry into one instanced
/// draw, and submit the lot as indirect commands. Nothing here knows what the
/// view is for — a cascade, a spot light's map and one face of a point light
/// are the same call with a different matrix and a different rectangle.
///
/// Every view of a frame is built and uploaded together, in one instance buffer
/// and one indirect buffer, with each view's commands occupying its own range.
/// A frame therefore uploads once however many views it draws, which is what
/// keeps the per-view cost to the draw itself.
///
/// Two things deliberately stay outside: the pipeline, because its raster state
/// and target format belong to whoever owns the target, and clearing, because a
/// cascade clears every frame while a cached atlas tile must specifically not.
///
/// A view draws in two halves. Casters with no alpha to test go through a
/// pipeline with no fragment stage at all, which is what makes four cascades
/// affordable; cutouts go through one that samples base-colour alpha and
/// discards, so their shadow carries the hole. The split is the point — one
/// shader for both would charge every opaque caster a texture fetch to cut
/// holes in geometry that has none.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/ShadowView.hpp>

namespace Assisi::Render
{

/// @brief Identity of one drawable submesh: the mesh's process-unique id and
/// which of its submeshes.
///
/// Orders a caster span geometry-major, and consecutive equal keys are what
/// coalesce into a single instanced draw. The mesh id rather than its address,
/// so the ordering is the same every frame whatever the allocator did.
[[nodiscard]] constexpr std::uint64_t ShadowGeometryKey(std::uint32_t meshId, std::uint32_t submeshIndex)
{
    return (static_cast<std::uint64_t>(meshId) << 32) | submeshIndex;
}

/// @brief One shadow-casting submesh instance, resolved to what a draw needs.
///
/// The geometry is resolved once where the casters are gathered rather than
/// looked up again per view: a caster survives into several views, and the
/// lookup would otherwise repeat once for each of them.
struct ShadowCaster
{
    std::uint64_t geometryKey = 0;

    // The submesh's range in its arena. Read where the caster is gathered, in
    // the same frame it is drawn, so an arena grow cannot land in between.
    nvrhi::IBuffer *vertexBuffer = nullptr;
    nvrhi::IBuffer *indexBuffer = nullptr;
    std::uint32_t indexCount = 0;
    std::uint32_t startIndexLocation = 0;
    std::int32_t baseVertexLocation = 0;

    glm::mat4 model{1.f};

    /// Carried rather than recomputed because the gather already has it from
    /// the mesh's local bounds and the instance's matrix, and every view tests
    /// against it.
    Geometry::BoundingSphere worldSphere;

    /// Which views this caster can reach: bit i is target i of the span the
    /// draw list is built over. The gather sets it, which is where the world
    /// sphere is already in hand; a caller that classifies nothing leaves every
    /// bit set and every view walks every caster, as they did before there were
    /// masks.
    ///
    /// It only ever removes work. The volume each bit is decided against
    /// contains the view's whole ortho box, so a cleared bit is a caster the
    /// view's own frustum test would have rejected anyway — which is what makes
    /// this a filter on the sweep rather than a second, weaker cull with its own
    /// answer.
    std::uint32_t viewMask = ~0u;

    /// Whether this caster's material alpha-tests, and so draws through a
    /// fragment stage that can discard. Opaque casters must keep the pipeline
    /// with no fragment stage at all — charging every one of them a texture
    /// fetch to cut holes in geometry that has none is the cost this separation
    /// exists to avoid.
    bool alphaMasked = false;

    /// Whether this caster's material is drawn from both sides, and so must be
    /// recorded from both. A single-sided caster is a closed shell whose back
    /// faces are its interior, and culling them is free and correct; a
    /// double-sided one is a surface with no interior at all, and culling either
    /// face of it drops the caster whenever its winding happens to face away.
    ///
    /// The mesh pass already decides this the same way, from the same flag. The
    /// depth pass reading it too is what keeps the two from disagreeing about
    /// what a piece of geometry is.
    bool doubleSided = false;

    /// Row into the material table, where the alpha-testing fragment stage
    /// finds the base-colour slot and the cutoff. Unread for an opaque caster.
    std::uint32_t materialIndex = 0;
};

/// @brief Whether two casters draw the same geometry, and so can be submitted
/// as one instanced draw.
///
/// The key alone answers this while every mesh carries a distinct id, which is
/// what AssetCache hands out. The draw range is compared as well because the
/// cost is a handful of integer compares and the failure it prevents is silent:
/// two casters wrongly merged draw one's geometry at the other's place, with
/// nothing anywhere reporting that anything went wrong.
///
/// The alpha test is part of the answer because it decides the pipeline: an
/// opaque and a cutout draw of one submesh cannot share a command without one
/// of them being submitted through the other's fragment stage.
[[nodiscard]] inline bool SameShadowGeometry(const ShadowCaster &lhs, const ShadowCaster &rhs)
{
    return lhs.geometryKey == rhs.geometryKey && lhs.indexCount == rhs.indexCount &&
           lhs.startIndexLocation == rhs.startIndexLocation && lhs.baseVertexLocation == rhs.baseVertexLocation &&
           lhs.vertexBuffer == rhs.vertexBuffer && lhs.indexBuffer == rhs.indexBuffer &&
           lhs.alphaMasked == rhs.alphaMasked && lhs.doubleSided == rhs.doubleSided;
}

/// @brief One per-object record: the world matrix, and the material row the
/// alpha test reads.
///
/// One record shape for both pipelines rather than two, because a frame's views
/// share a single instance buffer and a second stride would need a second one.
/// The opaque vertex stage declares the material index and never reads it.
struct ShadowInstanceData
{
    glm::mat4 model{1.f};
    std::uint32_t materialIndex = 0;
    std::uint32_t _pad0 = 0, _pad1 = 0, _pad2 = 0;
};
static_assert(sizeof(ShadowInstanceData) == 80, "ShadowInstanceData must match the shader's std430 array stride.");

/// @brief One view and the target it draws into.
struct ShadowDepthTarget
{
    ShadowView view;
    /// The framebuffer the view's rectangle is a rectangle of. Never read while
    /// the draw list is built — only when it is submitted.
    nvrhi::IFramebuffer *framebuffer = nullptr;
};

/// @brief The instances and indirect commands one frame's views submit.
///
/// Kept across frames and refilled, so a steady state allocates nothing.
struct ShadowDrawList
{
    /// Every view's per-object records, concatenated. A command's
    /// startInstanceLocation indexes this.
    std::vector<ShadowInstanceData> instances;
    std::vector<nvrhi::DrawIndexedIndirectArguments> commands;
    /// The buffers each command draws from, parallel to @ref commands. A run of
    /// commands sharing both binds once and submits as one multi-draw.
    std::vector<nvrhi::IBuffer *> commandVertexBuffers;
    std::vector<nvrhi::IBuffer *> commandIndexBuffers;
    /// Where each view's run of commands starts, with a final entry holding the
    /// total — so view i owns [viewCommandStart[i], viewCommandStart[i + 1]).
    std::vector<std::uint32_t> viewCommandStart;
    /// Where each view's run of each pipeline class begins: kMeshPipelineCount
    /// entries per view, in MeshPipeline's own order, so view i's class c owns
    /// [viewPipelineStart[i * kMeshPipelineCount + c], next), and the last
    /// class ends at viewCommandStart[i + 1].
    ///
    /// Four rather than two because a caster's cull mode is pipeline state as
    /// much as its fragment stage is, and the two vary independently. Empty
    /// classes cost an equal pair of bounds and no draw, which is every class
    /// but the first in a scene of ordinary solid geometry.
    std::vector<std::uint32_t> viewPipelineStart;
    /// Caster-view pairs no view drew, whichever cull rejected them: the mask
    /// before the walk, or the frustum test during it.
    std::uint32_t culled = 0;

    /// Indices into the caster span of every view's members, concatenated, in
    /// the span's own order — so a view's list is class-major and geometry-major
    /// exactly as the span is, and the runs still coalesce. View i owns
    /// [viewMemberStart[i], viewMemberStart[i + 1]).
    std::vector<std::uint32_t> viewMembers;
    std::vector<std::uint32_t> viewMemberStart;
    /// Where the next member of each view goes while the lists are being
    /// filled. A member of the struct rather than a local so a steady state
    /// allocates nothing here either.
    std::vector<std::uint32_t> viewMemberCursor;

    void Clear();
};

/// @brief Cull @p casters against every target's view and build the draw list.
///
/// @p casters is expected sorted opaque-first and by @ref ShadowGeometryKey
/// within each half — consecutive items with the same key coalesce into one
/// instanced draw, and an unsorted span merely produces more commands for the
/// same picture. A caster the frustum test rejects also breaks the run, so the
/// next survivor opens a new batch. The two halves are separated here whatever
/// the ordering, so only the coalescing depends on it and never the picture.
///
/// Each view walks only the members its casters' @ref ShadowCaster::viewMask
/// named, rather than the whole span once per pipeline class: the classification
/// happens once per caster where its sphere is already in hand, and this costs
/// what survives it. A caster the mask kept out of a view is not in that view's
/// list at all, so its neighbours coalesce across it — the view's instances are
/// appended in its own member order, and a command's range covers its members
/// and nothing between them.
///
/// Device-free by construction: this is the whole of what the depth pass
/// decides, and none of it needs a GPU to be checked.
void BuildShadowDrawList(std::span<const ShadowDepthTarget> targets, std::span<const ShadowCaster> casters,
                         ShadowDrawList &out);

/// @brief The pipeline a view draws each class of caster through, indexed by
/// MeshPipeline.
///
/// A null masked entry means alpha-tested casters fall back to the opaque
/// pipeline of the same cull mode — a solid silhouette, which is what a build
/// without the alpha-testing variant gets. Losing the hole is worse than losing
/// the shadow, so the caster is never simply dropped.
struct ShadowPipelines
{
    std::array<nvrhi::IGraphicsPipeline *, kMeshPipelineCount> byPipeline{};

    [[nodiscard]] nvrhi::IGraphicsPipeline *For(MeshPipeline pipeline) const
    {
        return byPipeline[static_cast<std::uint32_t>(pipeline)];
    }
};

/// @brief The depth-only pipeline and the buffers that feed it, shared by every
/// kind of shadow map.
class ShadowDepthRenderer
{
public:
    ShadowDepthRenderer() = default;

    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// Compiled-SPIR-V path of the depth-only vertex stage.
        std::string vertexShaderSpvPath = {};
        /// The alpha-testing variant: a vertex stage that also carries the UV
        /// and the material row, and the fragment stage that discards on them.
        /// Leaving either empty, or either failing to load, leaves the renderer
        /// working with alpha-tested casters drawing solid.
        std::string maskedVertexShaderSpvPath = {};
        std::string maskedPixelShaderSpvPath = {};
        /// The AssetCache's material table and bindless texture table, which the
        /// alpha test reads the cutoff and the base-colour texture from. Not
        /// owned, and both must outlive the renderer. Absent, the alpha-testing
        /// pipeline is not built.
        nvrhi::IBuffer *materialTable = nullptr;
        nvrhi::IBindingLayout *bindlessLayout = nullptr;
        nvrhi::IDescriptorTable *bindlessTable = nullptr;
    };

    /// @brief Load the vertex stage and build the layouts every pipeline shares.
    /// @return false if the shader or a layout failed, which leaves the renderer
    /// permanently unusable rather than failing the frame.
    [[nodiscard]] bool Initialize(const InitParams &params);

    [[nodiscard]] bool IsReady() const { return _device != nullptr && _vertexShader != nullptr; }

    /// @brief Whether the alpha-testing variant loaded, and so whether
    /// CreateMaskedPipeline returns anything.
    [[nodiscard]] bool CanAlphaTest() const;

    /// @brief A depth-only pipeline for targets shaped like @p prototype.
    ///
    /// Handed back rather than held, because the raster state and the target's
    /// depth format belong to whoever owns the target: the sun's cascades and a
    /// local-light atlas differ in both and would otherwise thrash one pipeline
    /// between them.
    ///
    /// Back faces are culled, always. Keeping the front ones instead was once
    /// the way to stop a surface shadowing itself, and it worked by recording
    /// the far side of the geometry — which displaces every shadow by the
    /// caster's own thickness and drops single-sided casters entirely. The slope
    /// bias here and the normal offset the mesh shader applies cover the same
    /// self-shadowing without displacing anything, so there is no trade to expose.
    ///
    /// @p slopeBias multiplies the polygon's own depth slope; @p slopeBiasClamp
    /// caps the product, in the same [0, 1] depth the map stores. The cap is the
    /// widest gap this side can open under a silhouette, so it belongs to the
    /// map's resolution — see SlopeBiasClampNdc.
    /// @p pipeline selects the fragment stage and the cull mode together: a
    /// masked class carries the alpha test, a double-sided class rasterizes both
    /// faces. Returns null for a masked class when the alpha-testing variant did
    /// not load, which the caller falls back from rather than dropping casters.
    [[nodiscard]] nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IFramebuffer *prototype, MeshPipeline pipeline,
                                                               float slopeBias, float slopeBiasClamp) const;

    /// @brief Start a frame's view table.
    ///
    /// Every Render() until the next call appends its views to one table, so a
    /// single buffer describes every shadow view of the frame however many
    /// kinds of shadow map produced them — and a shader that samples one of
    /// them indexes the same table whichever kind it was.
    void BeginFrame();

    /// @brief The frame's view table, or null before anything has been drawn
    /// into it.
    [[nodiscard]] nvrhi::IBuffer *ViewTable() const { return _viewTable.NativeBuffer(); }

    /// @brief How many views the table holds.
    [[nodiscard]] std::uint32_t ViewCount() const { return static_cast<std::uint32_t>(_views.size()); }

    /// @brief What one Render() drew, summed over its views.
    struct Stats
    {
        /// Index of this call's first view in the frame's table.
        std::uint32_t firstView = 0;
        std::uint32_t views = 0;     ///< Views rendered.
        std::uint32_t instances = 0; ///< Caster instances submitted, counted once per view they survive into.
        std::uint32_t batches = 0;   ///< Instanced draw commands after coalescing same-geometry runs.
        /// How many of @ref batches went through the alpha-testing pipeline.
        /// Zero is what a scene with no cutout in it reports, which is how the
        /// no-regression claim is checked rather than eyeballed.
        std::uint32_t maskedBatches = 0;
        std::uint32_t drawCalls = 0; ///< drawIndexedIndirect calls issued.
        std::uint32_t culled = 0;    ///< Caster-view pairs no view drew, mask and frustum test together.
    };

    /// @brief Draw @p casters into every target, with @p pipelines.
    ///
    /// The targets are not cleared: that is the target owner's policy, and it
    /// differs between a cascade redrawn every frame and an atlas tile kept
    /// from the last one.
    Stats Render(nvrhi::ICommandList *commandList, const ShadowPipelines &pipelines,
                 std::span<const ShadowDepthTarget> targets, std::span<const ShadowCaster> casters) const;

private:
    /// @brief Load the alpha-testing variant and build what only it needs.
    /// Never fails the renderer: what it cannot build leaves alpha-tested
    /// casters drawing through the opaque pipeline.
    void InitializeAlphaTest(const InitParams &params);

    [[nodiscard]] nvrhi::IBindingSet *GetOrCreateBindingSet(nvrhi::IBuffer *instanceBuffer) const;
    [[nodiscard]] nvrhi::IBindingSet *GetOrCreateMaskedBindingSet(nvrhi::IBuffer *instanceBuffer) const;
    void EnsureIndirectCapacity(std::uint32_t commandCount) const;

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::InputLayoutHandle _inputLayout;
    nvrhi::BindingLayoutHandle _bindingLayout;

    // The alpha-testing variant. Every handle here is null when the variant did
    // not load, which is what CanAlphaTest reports.
    nvrhi::ShaderHandle _maskedVertexShader;
    nvrhi::ShaderHandle _maskedPixelShader;
    nvrhi::InputLayoutHandle _maskedInputLayout;
    nvrhi::BindingLayoutHandle _maskedBindingLayout;
    nvrhi::SamplerHandle _maskedSampler;
    nvrhi::IBuffer *_materialTable = nullptr;
    nvrhi::BindingLayoutHandle _bindlessLayout;
    nvrhi::DescriptorTableHandle _bindlessTable;

    // Per-instance world matrices for every view in one buffer, rebuilt each
    // frame; grown geometrically, which swaps the handle and so invalidates the
    // cached binding set (GetOrCreateBindingSet notices).
    mutable Buffer _instanceBuffer;
    mutable nvrhi::BindingSetHandle _bindingSet;
    mutable const nvrhi::IBuffer *_bindingSetInstanceBuffer = nullptr;
    mutable nvrhi::BindingSetHandle _maskedBindingSet;
    mutable const nvrhi::IBuffer *_maskedBindingSetInstanceBuffer = nullptr;

    mutable nvrhi::BufferHandle _indirectBuffer;
    mutable std::uint32_t _indirectCapacity = 0; // in commands

    // The frame's views, accumulated across every Render() since BeginFrame and
    // uploaded whole each time, so the table is complete after any of them.
    mutable std::vector<ShadowViewGpu> _views;
    mutable Buffer _viewTable;

    mutable ShadowDrawList _drawList;
};

} // namespace Assisi::Render
