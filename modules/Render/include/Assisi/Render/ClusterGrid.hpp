/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ClusterGrid.hpp
/// @brief Clustered forward lighting pipeline.
///
/// Divides the view frustum into a 3-D grid (16 × 9 × 24 = 3 456 clusters).
/// A compute pass builds view-space AABBs once per resize, and a second
/// compute pass assigns lights to clusters every frame.  The resulting
/// buffers are read directly by the mesh fragment shader (see MeshPass /
/// cube_min.frag) so it only evaluates the lights that touch each
/// fragment's cluster.
///
/// Usage (per frame):
///   1. grid.CullLights(commandList, pointLights, spotLights, dirLights, viewMatrix);
///   2. DrawScene(...)  — cube_min.frag reads the buffers step 1 just wrote.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/ComputeShader.hpp>

#include <cstdint>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

// ---- GPU-side light structs (std430 layout) --------------------------------
// All vec3 fields are stored in vec4 to satisfy std430 alignment rules.
// The C++ structs must have identical memory layout to the GLSL structs in
// cluster_cull.comp / cube_min.frag.

struct PointLightGPU
{
    glm::vec4 positionRadius; ///< xyz = world position, w = influence radius
    glm::vec4 colorIntensity; ///< xyz = linear-RGB colour,  w = intensity
};
static_assert(sizeof(PointLightGPU) == 32);

struct SpotLightGPU
{
    glm::vec4 positionRadius; ///< xyz = world position,       w = influence radius
    glm::vec4 directionInner; ///< xyz = direction (unit vec),  w = cos(innerAngle)
    glm::vec4 colorIntensity; ///< xyz = linear-RGB colour,     w = intensity
    float     outerCutoff;    ///< cos(outerAngle)
    float     _pad[3]{};
};
static_assert(sizeof(SpotLightGPU) == 64);

struct DirLightGPU
{
    glm::vec4 directionIntensity; ///< xyz = direction toward light (unit vec), w = intensity
    glm::vec4 colorPad;           ///< xyz = linear-RGB colour, w = unused
};
static_assert(sizeof(DirLightGPU) == 32);

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

    /// Maximum light indices stored in the global list, per light type.
    /// Point indices occupy [0, kMaxLightIndices) and spot indices occupy
    /// [kMaxLightIndices, 2 * kMaxLightIndices) in the same buffer. Must match
    /// cluster_cull.comp's MAX_LIGHT_INDICES and cube_min.frag's kSpotIndexBase.
    ///
    /// Sized generously: with the per-cluster cap removed (cluster_cull.comp
    /// writes every intersecting light, not a fixed 64), the total across all
    /// clusters is the only bound. A cluster whose reservation lands past the
    /// end is clamped in the shader, so an overflow degrades gracefully (a few
    /// distant lights drop) rather than writing out of bounds.
    static constexpr uint32_t kMaxLightIndices = 262144u;

    /// Fixed light-data buffer capacities. Lights beyond these caps are
    /// silently dropped by Buffer::Upload — generous enough for any scene
    /// this engine currently loads, and avoids reallocating buffers every
    /// frame the way the old OpenGL SSBOs did.
    static constexpr uint32_t kMaxPointLights = 1024u;
    static constexpr uint32_t kMaxSpotLights  = 1024u;
    static constexpr uint32_t kMaxDirLights   = 16u;

    ClusterGrid() = default;

    /// @brief Loads compute shaders and allocates all buffers.
    /// @return false if either compute shader failed to compile.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device);

    /// @brief Rebuild cluster AABBs. Call once on init and again on viewport/projection change.
    void BuildClusters(nvrhi::ICommandList *commandList, int width, int height, float nearZ, float farZ,
                       const glm::mat4 &invProjection);

    /// @brief Upload lights to buffers and run the culling compute pass.
    void CullLights(nvrhi::ICommandList *commandList, const std::vector<PointLightGPU> &pointLights,
                    const std::vector<SpotLightGPU> &spotLights, const std::vector<DirLightGPU> &dirLights,
                    const glm::mat4 &view);

    float NearZ() const { return _nearZ; }
    float FarZ()  const { return _farZ; }
    int   Width() const { return _width; }
    int   Height() const { return _height; }

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
    int   _width  = 0;
    int   _height = 0;
};

} // namespace Assisi::Render
