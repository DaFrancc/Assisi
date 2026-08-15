/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DrawItem.hpp
/// @brief One drawable submesh instance + the key it sorts by — the mesh pass's
///        producer/consumer seam (docs/mesh-material-architecture.md §5).
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

/// @brief A single submesh of a single entity, ready to record. Non-owning:
/// `mesh`/`material` point into the AssetCache and stay valid until it clears;
/// `model` is the entity's world matrix (the MVP is derived in Submit).
struct DrawItem
{
    uint64_t sortKey      = 0;
    const MeshBuffer *mesh         = nullptr;
    uint32_t submeshIndex = 0;          ///< Index into mesh->SubMeshes().
    const Material *material     = nullptr;
    glm::mat4 model{1.f};
};

// --- Opaque sort key: [pipeline:8 | materialId:20 | meshId:20 | depth:16] ----
//
// Material-major then mesh-major keeps binding-set and vertex/index-buffer
// changes to the run boundaries, and puts same-geometry draws adjacent so Submit
// coalesces them into instanced commands; the low 16 depth bits break ties
// front-to-back within a run for early-Z. The transparent pass will use a
// separate depth-major key later (§5) — hence "opaque".
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
[[nodiscard]] inline uint64_t MakeOpaqueSortKey(uint32_t pipeline, uint32_t materialId, uint32_t meshId,
                                                uint16_t depth)
{
    const uint64_t pipelineBits = static_cast<uint64_t>(pipeline & ((1u << kSortPipelineBits) - 1))
                                  << (kSortMaterialBits + kSortMeshBits + kSortDepthBits);
    const uint64_t materialBits = static_cast<uint64_t>(materialId & kSortMaterialMax)
                                  << (kSortMeshBits + kSortDepthBits);
    const uint64_t meshBits = static_cast<uint64_t>(meshId & kSortMeshMax) << kSortDepthBits;
    return pipelineBits | materialBits | meshBits | static_cast<uint64_t>(depth);
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
