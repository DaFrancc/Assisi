/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WaterGrid.hpp
/// @brief The mesh a water surface is drawn with: one square patch of grid,
/// drawn many times at doubling sizes, finest around the camera.
///
/// Which patches are drawn is a quadtree walk on the CPU each frame. A node is
/// split into its four children while the camera is nearer it than its
/// children's range, so the levels fall away in rings with distance and two
/// neighbours never differ by more than one. Near the outer edge of its range a
/// patch's odd vertices slide onto their even neighbours, so at the boundary it
/// has the same vertices as the coarser patch beside it and no crack opens.
/// That is Strugar's continuous distance-dependent level of detail (CDLOD,
/// 2010), here over a flat plane.
///
/// Nodes sit on a grid fixed to the water body, so a vertex never slides across
/// the waves as the camera moves; only a whole patch changes level.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Frustum.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::Render
{

/// @brief Quads along each side of the patch.
inline constexpr uint32_t kWaterPatchQuads = 32u;

/// @brief Metres between the vertices of the finest level.
inline constexpr float kWaterFinestSpacing = 0.25f;

/// @brief A level is drawn out to this many of its own node sizes from the
/// camera.
///
/// Where a patch meets a coarser one, the coarser must not have begun to blend.
/// That edge lies on the finer patch's parent, which is within one parent
/// diagonal (2√2 node sizes) of the range that split it, so the range r must
/// satisfy r + 2√2·size ≤ kWaterMorphStart·2r. This is the smallest round
/// number that does, which keeps the patch count, about thirty a level, down.
inline constexpr float kWaterRangeScale = 3.75f;

/// @brief A patch starts blending toward the coarser level at this share of
/// its range, and is wholly the coarser level at the range itself.
inline constexpr float kWaterMorphStart = 0.9f;

/// @brief The most levels a walk descends.
inline constexpr uint32_t kMaxWaterLevels = 16u;

/// @brief Where a body of water lies: its centre and height, which way its own
/// x axis points, and how far it reaches.
struct WaterRegion
{
    /// The centre, in the world; y is the water's height.
    glm::dvec3 origin{0.0};
    /// The body's x axis across the world, along world x and z: a unit vector.
    /// Its z axis is this turned a quarter to the right, seen from above.
    glm::vec2 axisX{1.0f, 0.0f};
    /// Half its width along its own x and z, in metres. Zero is endless.
    glm::vec2 halfExtent{0.0f};
};

/// @brief What one frame's walk is measured from.
struct WaterGridView
{
    glm::dvec3 camera{0.0};
    /// The frustum, measured from the camera; null draws every node in range.
    const Frustum *frustum = nullptr;
    /// Nothing further than this is drawn, in metres.
    float drawDistance = 0.0f;
    /// How far the waves can move the surface from its plane in any direction,
    /// in metres, so a node is not culled while its waves still show.
    float waveReach = 0.0f;
};

/// @brief One patch to draw.
struct WaterNode
{
    /// The corner nearest the body's negative x and z, in the body's frame,
    /// measured from the camera.
    glm::vec2 corner{0.0f};
    /// Its side, in metres.
    float size = 0.0f;
    uint32_t level = 0u;
};

/// @brief The side of a node at @p level, in metres.
[[nodiscard]] float WaterNodeSize(uint32_t level);

/// @brief How far from the camera @p level is drawn, in metres.
[[nodiscard]] float WaterLodRange(uint32_t level);

/// @brief How far a vertex @p distance metres from the camera, in a patch at
/// @p level, has blended toward the next level: 0 not at all, 1 wholly.
[[nodiscard]] float WaterMorph(float distance, uint32_t level);

/// @brief @p world measured in @p region's own frame from its centre, along
/// its x and z axes.
[[nodiscard]] glm::dvec2 RegionPosition(const WaterRegion &region, glm::dvec3 world);

/// @brief Replaces @p nodes with the patches that draw @p region from @p view.
void SelectWaterNodes(const WaterRegion &region, const WaterGridView &view, std::vector<WaterNode> &nodes);

} // namespace Assisi::Render
