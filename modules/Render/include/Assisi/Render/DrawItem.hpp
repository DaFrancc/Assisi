/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DrawItem.hpp
/// @brief One drawable submesh instance + the key it sorts by — the mesh pass's
///        producer/consumer seam.
///
/// A runtime *producer* extracts one DrawItem per visible submesh (cull → LOD
/// select → emit), sorts a span of them by `sortKey`, and hands that span to
/// MeshPass::Submit — the *consumer*. Sorting by the key groups draws into
/// pipeline / material / mesh runs, and orders front-to-back within a run for
/// early-Z. Submit turns those runs into instanced indirect commands;
/// the producer knows nothing about how it does it.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Render
{
class MeshBuffer;
class Material;

/// @brief Which mesh-pass pipeline records a draw.
///
/// Masked draws need a pixel shader that can `discard`, and a shader that can
/// discard costs its whole pipeline early depth rejection — so the cutout
/// materials get a pipeline of their own rather than making every opaque draw
/// pay for them. This is the sort key's top field, so a sorted span arrives
/// already grouped into one run per pipeline, and Opaque leading means the solid
/// geometry has laid its depth before any cutout is tested against it.
enum class MeshPipeline : uint32_t
{
    Opaque = 0, ///< AlphaMode::Opaque materials.
    Mask   = 1, ///< AlphaMode::Mask materials; its shader can discard.
};

/// @brief How many pipelines MeshPipeline names — the size of every per-pipeline
/// array. Extending the enum without extending this would silently index past one.
inline constexpr uint32_t kMeshPipelineCount = 2;

/// @brief A single submesh of a single entity, ready to record. Non-owning:
/// `mesh`/`material` point into the AssetCache and stay valid until it clears;
/// `model` is the entity's world matrix (the MVP is derived in Submit).
struct DrawItem
{
    uint64_t sortKey      = 0;
    const MeshBuffer *mesh         = nullptr;
    uint32_t submeshIndex = 0;          ///< Index into mesh->SubMeshes().
    /// Whether a shadow pass should draw this item. Sits in the padding after
    /// `submeshIndex`, so carrying it costs nothing. It is not part of `sortKey`:
    /// the main pass draws casters and non-casters alike, so splitting runs on it
    /// would only break up material batches.
    bool castsShadows = true;
    const Material *material     = nullptr;
    glm::mat4 model{1.f};
};

// --- Opaque sort key: [pipeline:8 | materialId:20 | meshId:20 | depth:16] ----
//
// Pipeline-major first, so the pass binds each pipeline once and every masked
// draw follows every opaque one. Material-major then mesh-major within that keeps
// binding-set and vertex/index-buffer changes to the run boundaries, and puts
// same-geometry draws adjacent so Submit coalesces them into instanced commands;
// the low 16 depth bits break ties front-to-back within a run for early-Z. A
// blended pass would need a separate depth-major key — hence "opaque".
inline constexpr uint32_t kSortPipelineBits = 8;
inline constexpr uint32_t kSortMaterialBits = 20;
inline constexpr uint32_t kSortMeshBits     = 20;
inline constexpr uint32_t kSortDepthBits    = 16;
static_assert(kSortPipelineBits + kSortMaterialBits + kSortMeshBits + kSortDepthBits == 64);

inline constexpr uint32_t kSortMaterialMax = (1u << kSortMaterialBits) - 1; ///< ~1M distinct materials.
inline constexpr uint32_t kSortMeshMax     = (1u << kSortMeshBits) - 1;     ///< ~1M distinct meshes.

/// @brief Pack an opaque draw sort key. @p materialId / @p meshId are masked to
/// their field widths (ids run to ~1M before aliasing — far past any real scene;
/// the caller may assert before this if it wants to catch overflow). @p depth is
/// a front-to-back-quantized view distance (see QuantizeDepthFrontToBack).
[[nodiscard]] inline uint64_t MakeOpaqueSortKey(MeshPipeline pipeline, uint32_t materialId, uint32_t meshId,
                                                uint16_t depth)
{
    static_assert(kMeshPipelineCount <= (1u << kSortPipelineBits),
                  "the pipeline field must hold every MeshPipeline, or two would sort as one");
    const uint64_t pipelineBits = static_cast<uint64_t>(pipeline)
                                  << (kSortMaterialBits + kSortMeshBits + kSortDepthBits);
    const uint64_t materialBits = static_cast<uint64_t>(materialId & kSortMaterialMax)
                                  << (kSortMeshBits + kSortDepthBits);
    const uint64_t meshBits = static_cast<uint64_t>(meshId & kSortMeshMax) << kSortDepthBits;
    return pipelineBits | materialBits | meshBits | static_cast<uint64_t>(depth);
}

/// @brief The pipeline a sort key was packed with. Submit reads it back off the
/// key to know which pipeline a run of draws needs.
[[nodiscard]] inline MeshPipeline SortKeyPipeline(uint64_t sortKey)
{
    return static_cast<MeshPipeline>(sortKey >> (kSortMaterialBits + kSortMeshBits + kSortDepthBits));
}

/// @brief Quantize a positive view-space distance into the 16-bit depth field,
/// front (near) → 0, back (far) → 65535, so ascending sort is front-to-back.
/// @p viewDistance is clamped to [nearZ, farZ]; a degenerate range maps to 0.
[[nodiscard]] inline uint16_t QuantizeDepthFrontToBack(float viewDistance, float nearZ, float farZ)
{
    constexpr float kDepthMax = 65535.f;
    const float span      = farZ - nearZ;
    if (span <= 0.f)
    {
        return 0;
    }
    const float normalized = std::clamp((viewDistance - nearZ) / span, 0.f, 1.f);
    return static_cast<uint16_t>(std::lround(normalized * kDepthMax));
}

} /* namespace Assisi::Render */
