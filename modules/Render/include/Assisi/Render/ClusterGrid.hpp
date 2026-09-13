/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ClusterGrid.hpp
/// @brief Clustered forward lighting pipeline.
///
/// Divides the view frustum into a 3-D grid (16 × 9 × 24 = 3 456 clusters).
/// A compute pass builds view-space AABBs once per resize, and a second
/// compute pass assigns lights to clusters every frame.  The resulting
/// buffers are read directly by the mesh fragment shader (see MeshPass /
/// mesh.frag) so it only evaluates the lights that touch each
/// fragment's cluster.
///
/// Usage (per frame):
///   1. grid.CullLights(commandList, pointLights, spotLights, dirLights, viewMatrix);
///   2. DrawScene(...)  — mesh.frag reads the buffers step 1 just wrote.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/ComputeShader.hpp>
#include <Assisi/Render/GpuLayout.hpp>

#include <cstdint>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

// ---- GPU-side light structs (std430 layout) --------------------------------
// All vec3 fields are stored in vec4 to satisfy std430 alignment rules.
// The C++ structs must have identical memory layout to the GLSL structs in
// cluster_cull.comp / mesh.frag.
//
// Each one declares where every member lands (see GpuLayout.hpp). That is the
// whole defence: a member inserted, reordered or resized here and not in the
// shader does not fail to build and does not fail to run — the shader reads the
// wrong sixteen bytes and the picture is subtly wrong, which is the hardest kind
// of mistake to find. Declared offsets turn it into a compile error.

/// @brief What a light's `shadowView` holds when it has no atlas tile.
///
/// Every light carries the lane whether or not it shadows, because the alternative
/// is a second buffer the shader would have to index in parallel. A sentinel
/// costs a compare the shader was going to make anyway — it has to know whether
/// to sample at all.
inline constexpr uint32_t kNoShadowView = 0xFFFFFFFFu;

struct PointLightGPU
{
    glm::vec4 positionRadius; ///< xyz = world position, w = influence radius
    glm::vec4 colorIntensity; ///< xyz = linear-RGB colour,  w = intensity
    /// x = index of this light's first shadow view in the frame's view table, or
    /// kNoShadowView. Its six faces are the next six entries, in the cubemap
    /// order ShadowView.hpp's PointLightFaceOf selects with. yzw unused.
    glm::uvec4 shadowView{kNoShadowView, 0u, 0u, 0u};
};
ASSISI_GPU_LAYOUT(PointLightGPU);
ASSISI_GPU_FIRST_FIELD(PointLightGPU, positionRadius);
ASSISI_GPU_FIELD_AFTER(PointLightGPU, colorIntensity, positionRadius);
ASSISI_GPU_FIELD_AFTER(PointLightGPU, shadowView, colorIntensity);
ASSISI_GPU_NO_TAIL_PADDING(PointLightGPU, shadowView);

struct SpotLightGPU
{
    glm::vec4 positionRadius; ///< xyz = world position,       w = influence radius
    glm::vec4 directionInner; ///< xyz = direction (unit vec),  w = cos(innerAngle)
    glm::vec4 colorIntensity; ///< xyz = linear-RGB colour,     w = intensity
    float outerCutoff;        ///< cos(outerAngle)
    /// This light's single shadow view, or kNoShadowView. It rides in what was
    /// padding, so carrying it costs the buffer nothing.
    uint32_t shadowView = kNoShadowView;
    /// std430 rounds a struct up to its largest member's alignment, which for a
    /// vec4 is 16 — so the shader's stride is 64 and the last two lanes exist
    /// only to reach it.
    ///
    /// Written out rather than left to the compiler, because GLM's default
    /// gentypes are 4-aligned: nothing here would round the struct up on its
    /// own, and without these the C++ side would be 56 bytes against the
    /// shader's 64. The assert below is what says so.
    float _pad[2]{};
};
ASSISI_GPU_LAYOUT(SpotLightGPU);
ASSISI_GPU_FIRST_FIELD(SpotLightGPU, positionRadius);
ASSISI_GPU_FIELD_AFTER(SpotLightGPU, directionInner, positionRadius);
ASSISI_GPU_FIELD_AFTER(SpotLightGPU, colorIntensity, directionInner);
ASSISI_GPU_FIELD_AFTER(SpotLightGPU, outerCutoff, colorIntensity);
ASSISI_GPU_FIELD_AFTER(SpotLightGPU, shadowView, outerCutoff);
ASSISI_GPU_FIELD_AFTER(SpotLightGPU, _pad, shadowView);
ASSISI_GPU_NO_TAIL_PADDING(SpotLightGPU, _pad);

struct DirLightGPU
{
    glm::vec4 directionIntensity; ///< xyz = direction the light TRAVELS (unit vec), w = intensity
    glm::vec4 colorPad;           ///< xyz = linear-RGB colour, w = unused
};
ASSISI_GPU_LAYOUT(DirLightGPU);
ASSISI_GPU_FIRST_FIELD(DirLightGPU, directionIntensity);
ASSISI_GPU_FIELD_AFTER(DirLightGPU, colorPad, directionIntensity);
ASSISI_GPU_NO_TAIL_PADDING(DirLightGPU, colorPad);

// ---------------------------------------------------------------------------

/// @brief Manages all buffers and compute shaders for clustered forward lighting.
class ClusterGrid
{
public:
    // ----- Grid constants --------------------------------------------------
    static constexpr uint32_t kNumX        = 16u;
    static constexpr uint32_t kNumY        = 9u;
    static constexpr uint32_t kNumZ        = 24u;
    static constexpr uint32_t kNumClusters = kNumX * kNumY * kNumZ; // 3 456

    /// Threads per workgroup in cluster_build.comp / cluster_cull.comp, and the
    /// batch size the cull streams lights through shared memory in. Both shaders
    /// hardcode it in their `layout(local_size_x = ...)`, so changing it here
    /// alone is not enough — grep for CULL_BATCH.
    static constexpr uint32_t kClusterGroupSize = 64u;

    /// Workgroups needed to cover the grid at one cluster per thread. The
    /// shaders bounds-check, so a grid size that is not a multiple of the group
    /// size is safe (the tail threads idle).
    static constexpr uint32_t kClusterDispatchGroups =
        (kNumClusters + kClusterGroupSize - 1u) / kClusterGroupSize; // 54

    /// Maximum light indices stored in the global list, per light type.
    /// Point indices occupy [0, kMaxLightIndices) and spot indices occupy
    /// [kMaxLightIndices, 2 * kMaxLightIndices) in the same buffer. Must match
    /// cluster_cull.comp's MAX_LIGHT_INDICES and mesh.frag's kSpotIndexBase.
    ///
    /// Sized generously: cluster_cull.comp writes every intersecting light with no
    /// per-cluster cap, so the total across all clusters is the only bound. A
    /// cluster whose reservation lands past the end is clamped in the shader, so an
    /// overflow degrades gracefully (a few distant lights drop) rather than writing
    /// out of bounds.
    static constexpr uint32_t kMaxLightIndices = 262144u;

    /// Fixed light-data buffer capacities, sized past any scene this engine loads
    /// so the buffers never resize. Lights beyond a cap are dropped by
    /// Buffer::Upload, which warns once per buffer when it truncates.
    static constexpr uint32_t kMaxPointLights = 1024u;
    static constexpr uint32_t kMaxSpotLights  = 1024u;
    static constexpr uint32_t kMaxDirLights   = 16u;

    ClusterGrid() = default;

    /// @brief Loads compute shaders and allocates all buffers.
    /// @return false if either compute shader failed to compile.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device);

    /// @brief Rebuild cluster AABBs. Call once on init and again on viewport/projection change.
    void BuildClusters(nvrhi::ICommandList *commandList, int32_t width, int32_t height, float nearZ, float farZ,
                       const glm::mat4 &invProjection);

    /// @brief Upload lights to buffers and run the culling compute pass.
    void CullLights(nvrhi::ICommandList *commandList, const std::vector<PointLightGPU> &pointLights,
                    const std::vector<SpotLightGPU> &spotLights, const std::vector<DirLightGPU> &dirLights,
                    const glm::mat4 &view);

    float NearZ() const { return _nearZ; }
    float FarZ()  const { return _farZ; }
    int32_t Width() const { return _width; }
    int32_t Height() const { return _height; }

    /// @brief Buffers the mesh fragment shader binds as SRVs each frame.
    ///@{
    const Buffer &PointLightBuffer()  const { return _pointLightBuffer; }
    const Buffer &SpotLightBuffer()   const { return _spotLightBuffer; }
    const Buffer &DirLightBuffer()    const { return _dirLightBuffer; }
    const Buffer &LightIndexBuffer()  const { return _lightIndexBuffer; }
    const Buffer &LightGridBuffer()   const { return _lightGridBuffer; }
    ///@}

private:
    nvrhi::IDevice *_device = nullptr;

    ComputeShader _buildShader;
    ComputeShader _cullShader;

    nvrhi::BindingSetHandle _buildBindingSet;
    nvrhi::BindingSetHandle _cullBindingSet;

    Buffer _clusterAABBBuffer;
    Buffer _pointLightBuffer;
    Buffer _spotLightBuffer;
    Buffer _dirLightBuffer;
    Buffer _lightIndexBuffer;
    Buffer _lightGridBuffer;
    Buffer _globalCountBuffer;

    float _nearZ  = 0.1f;
    float _farZ   = 200.f;
    int32_t _width  = 0;
    int32_t _height = 0;
};

} // namespace Assisi::Render
