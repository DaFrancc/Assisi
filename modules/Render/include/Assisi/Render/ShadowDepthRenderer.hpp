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

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
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
};

/// @brief Whether two casters draw the same geometry, and so can be submitted
/// as one instanced draw.
///
/// The key alone answers this while every mesh carries a distinct id, which is
/// what AssetCache hands out. The draw range is compared as well because the
/// cost is a handful of integer compares and the failure it prevents is silent:
/// two casters wrongly merged draw one's geometry at the other's place, with
/// nothing anywhere reporting that anything went wrong.
[[nodiscard]] inline bool SameShadowGeometry(const ShadowCaster &lhs, const ShadowCaster &rhs)
{
    return lhs.geometryKey == rhs.geometryKey && lhs.indexCount == rhs.indexCount &&
           lhs.startIndexLocation == rhs.startIndexLocation && lhs.baseVertexLocation == rhs.baseVertexLocation &&
           lhs.vertexBuffer == rhs.vertexBuffer && lhs.indexBuffer == rhs.indexBuffer;
}

/// @brief One per-object record. A bare world matrix: the depth pass has no
/// material, no normal and no texture coordinate to carry.
struct ShadowInstanceData
{
    glm::mat4 model{1.f};
};
static_assert(sizeof(ShadowInstanceData) == 64, "ShadowInstanceData must match the shader's std430 array stride.");

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
    /// Caster-view pairs the frustum test rejected.
    std::uint32_t culled = 0;

    void Clear();
};

/// @brief Cull @p casters against every target's view and build the draw list.
///
/// @p casters is expected sorted by @ref ShadowGeometryKey — consecutive items
/// with the same key coalesce into one instanced draw, and an unsorted span
/// merely produces more commands for the same picture. A caster the cull
/// rejects also breaks the run, so the next survivor opens a new batch.
///
/// Device-free by construction: this is the whole of what the depth pass
/// decides, and none of it needs a GPU to be checked.
void BuildShadowDrawList(std::span<const ShadowDepthTarget> targets, std::span<const ShadowCaster> casters,
                         ShadowDrawList &out);

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
    };

    /// @brief Load the vertex stage and build the layouts every pipeline shares.
    /// @return false if the shader or a layout failed, which leaves the renderer
    /// permanently unusable rather than failing the frame.
    [[nodiscard]] bool Initialize(const InitParams &params);

    [[nodiscard]] bool IsReady() const { return _device != nullptr && _vertexShader != nullptr; }

    /// @brief A depth-only pipeline for targets shaped like @p prototype.
    ///
    /// Handed back rather than held, because the raster state and the target's
    /// depth format belong to whoever owns the target: the sun's cascades and a
    /// local-light atlas differ in both and would otherwise thrash one pipeline
    /// between them.
    [[nodiscard]] nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IFramebuffer *prototype, float slopeBias,
                                                               bool cullFrontFaces) const;

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
        std::uint32_t drawCalls = 0; ///< drawIndexedIndirect calls issued.
        std::uint32_t culled = 0;    ///< Caster-view pairs the frustum test rejected.
    };

    /// @brief Draw @p casters into every target, with @p pipeline.
    ///
    /// The targets are not cleared: that is the target owner's policy, and it
    /// differs between a cascade redrawn every frame and an atlas tile kept
    /// from the last one.
    Stats Render(nvrhi::ICommandList *commandList, nvrhi::IGraphicsPipeline *pipeline,
                 std::span<const ShadowDepthTarget> targets, std::span<const ShadowCaster> casters) const;

private:
    [[nodiscard]] nvrhi::IBindingSet *GetOrCreateBindingSet(nvrhi::IBuffer *instanceBuffer) const;
    void EnsureIndirectCapacity(std::uint32_t commandCount) const;

    nvrhi::IDevice *_device = nullptr;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::InputLayoutHandle _inputLayout;
    nvrhi::BindingLayoutHandle _bindingLayout;

    // Per-instance world matrices for every view in one buffer, rebuilt each
    // frame; grown geometrically, which swaps the handle and so invalidates the
    // cached binding set (GetOrCreateBindingSet notices).
    mutable Buffer _instanceBuffer;
    mutable nvrhi::BindingSetHandle _bindingSet;
    mutable const nvrhi::IBuffer *_bindingSetInstanceBuffer = nullptr;

    mutable nvrhi::BufferHandle _indirectBuffer;
    mutable std::uint32_t _indirectCapacity = 0; // in commands

    // The frame's views, accumulated across every Render() since BeginFrame and
    // uploaded whole each time, so the table is complete after any of them.
    mutable std::vector<ShadowViewGpu> _views;
    mutable Buffer _viewTable;

    mutable ShadowDrawList _drawList;
};

} // namespace Assisi::Render
