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

/// @brief Which mesh-pass pipeline records a draw — one per combination of the
/// two pieces of per-draw state the pass cannot fold into a material row.
///
/// Masked draws need a pixel shader that can `discard`, and a shader that can
/// discard costs its whole pipeline early depth rejection — so the cutout
/// materials get their own pipelines rather than making every opaque draw pay.
/// Double-sided draws need the rasterizer's cull mode off, which is pipeline
/// state and not something a shader can vary.
///
/// The value is two bits: kMeshPipelineDoubleSidedBit and kMeshPipelineMaskedBit.
/// Masked is the *high* bit so every opaque pipeline sorts before every masked
/// one whatever their cull modes — this is the sort key's top field, so a sorted
/// span arrives grouped into one run per pipeline, with the solid geometry laying
/// its depth before any cutout is tested against it.
enum class MeshPipeline : uint32_t
{
    Opaque            = 0,
    OpaqueDoubleSided = 1,
    Mask              = 2, ///< Its shader can discard.
    MaskDoubleSided   = 3,
};

inline constexpr uint32_t kMeshPipelineDoubleSidedBit = 1u;
inline constexpr uint32_t kMeshPipelineMaskedBit      = 2u;

/// @brief How many pipelines MeshPipeline names — the size of every per-pipeline
/// array. Extending the enum without extending this would silently index past one.
inline constexpr uint32_t kMeshPipelineCount = 4;

/// @brief The pipeline a draw with these two properties belongs in. The only
/// place the mapping is written, so nothing can disagree about it.
[[nodiscard]] inline constexpr MeshPipeline MeshPipelineFor(bool masked, bool doubleSided)
{
    return static_cast<MeshPipeline>((masked ? kMeshPipelineMaskedBit : 0u) |
                                     (doubleSided ? kMeshPipelineDoubleSidedBit : 0u));
}

/// @brief Whether @p pipeline's shader carries the alpha-test discard.
[[nodiscard]] inline constexpr bool MeshPipelineIsMasked(MeshPipeline pipeline)
{
    return (static_cast<uint32_t>(pipeline) & kMeshPipelineMaskedBit) != 0u;
}

/// @brief Whether @p pipeline rasterizes back faces.
[[nodiscard]] inline constexpr bool MeshPipelineIsDoubleSided(MeshPipeline pipeline)
{
    return (static_cast<uint32_t>(pipeline) & kMeshPipelineDoubleSidedBit) != 0u;
}

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
