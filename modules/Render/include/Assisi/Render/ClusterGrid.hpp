/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ClusterGrid.hpp
/// @brief Clustered forward lighting pipeline.
///
/// Divides the view frustum into a 3-D grid (16 × 9 × 24 = 3 456 clusters).
/// A compute pass builds view-space AABBs once per resize, and a second
/// compute pass assigns lights to clusters every frame.  The resulting SSBOs
/// are bound to fixed binding points so the mesh fragment shader can look up
/// only the lights that touch each fragment's cluster.
///
/// Usage (per frame):
///   1. grid.CullLights(pointLights, spotLights, dirLights, viewMatrix);
///   2. DrawScene(...)  — mesh.frag reads SSBOs already bound by step 1.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ComputeShader.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::Render
{

// ---- GPU-side light structs (std430 layout) --------------------------------
// All vec3 fields are stored in vec4 to satisfy std430 alignment rules.
// The C++ structs must have identical memory layout.

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

/// @brief Manages all SSBOs and compute shaders for clustered forward lighting.
class ClusterGrid
{
  public:
    // ----- Grid constants --------------------------------------------------
    static constexpr unsigned int kNumX        = 16u;
    static constexpr unsigned int kNumY        = 9u;
    static constexpr unsigned int kNumZ        = 24u;
    static constexpr unsigned int kNumClusters = kNumX * kNumY * kNumZ; // 3 456

    /// Maximum light indices stored in the global list, per light type.
    /// Point indices occupy [0, kMaxLightIndices) and spot indices occupy
    /// [kMaxLightIndices, 2 * kMaxLightIndices) in the same buffer.
    static constexpr unsigned int kMaxLightIndices = 65536u;

    // ----- SSBO binding points (shared with all shaders) ------------------
    static constexpr unsigned int kBindingClusterAABB = 0u;
    static constexpr unsigned int kBindingPointLights = 1u;
    static constexpr unsigned int kBindingSpotLights  = 2u;
    static constexpr unsigned int kBindingDirLights   = 3u;
    static constexpr unsigned int kBindingLightIndex  = 4u;
    static constexpr unsigned int kBindingLightGrid   = 5u;
    static constexpr unsigned int kBindingGlobalCount = 6u;

    ClusterGrid() = default;
    ~ClusterGrid();

    ClusterGrid(const ClusterGrid &) = delete;
    ClusterGrid &operator=(const ClusterGrid &) = delete;

    ClusterGrid(ClusterGrid &&) noexcept;
    ClusterGrid &operator=(ClusterGrid &&) noexcept;

    /// @brief Load compute shaders and allocate all SSBOs.
    /// @return false if either compute shader failed to compile.
    bool Initialize();

    /// @brief Rebuild cluster AABBs. Call once on init and again on viewport/projection change.
    void BuildClusters(int width, int height, float nearZ, float farZ, const glm::mat4 &invProjection);

    /// @brief Upload lights to SSBOs and run the culling compute pass.
    ///        All SSBOs remain bound for subsequent DrawScene calls.
    void CullLights(const std::vector<PointLightGPU> &pointLights,
                    const std::vector<SpotLightGPU>  &spotLights,
                    const std::vector<DirLightGPU>   &dirLights,
                    const glm::mat4                  &view);

    float NearZ() const { return _nearZ; }
    float FarZ()  const { return _farZ; }
    int   Width() const { return _width; }
    int   Height() const { return _height; }

  private:
    void AllocateBuffers();
    void BindAll() const;
    void Destroy() noexcept;

    ComputeShader _buildShader;
    ComputeShader _cullShader;

    unsigned int _clusterAABBBuffer = 0u;
    unsigned int _pointLightBuffer  = 0u;
    unsigned int _spotLightBuffer   = 0u;
    unsigned int _dirLightBuffer    = 0u;
    unsigned int _lightIndexBuffer  = 0u;
    unsigned int _lightGridBuffer   = 0u;
    unsigned int _globalCountBuffer = 0u;

    float _nearZ  = 0.1f;
    float _farZ   = 200.f;
    int   _width  = 0;
    int   _height = 0;
};

} // namespace Assisi::Render