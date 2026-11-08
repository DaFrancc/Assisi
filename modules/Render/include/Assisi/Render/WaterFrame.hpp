/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WaterFrame.hpp
/// @brief The water one frame draws: each body in view, its waves and look,
/// and the patches that draw it.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Water.hpp>
#include <Assisi/Render/WaterGrid.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::Render
{

/// @brief How a body of water looks, apart from its waves.
struct WaterLook
{
    /// The colour light comes back from deep water in, linear.
    glm::vec3 color{0.0f};
    /// The colour of light that has crossed little water, which the crests
    /// show when the sun is behind them, linear.
    glm::vec3 crestColor{0.0f};
    /// How far light goes into the water before all but a third of it is
    /// gone, in metres. Shallower than this the ground shows through.
    float clarity = 1.0f;
};

/// @brief One body of water in view.
struct WaterBody
{
    WaterRegion region;
    /// Which body this is from frame to frame, so its wave textures are kept.
    uint64_t key = 0u;
    WaterCascades cascades;
    WaterWaves waves;
    WaterLook look;
    /// The significant height of all its waves together, in metres.
    float significantHeight = 0.0f;
    /// How far its waves move the surface from its plane, at most, in metres.
    float waveReach = 0.0f;
    /// Its patches, in WaterFrame::nodes.
    uint32_t firstNode = 0u;
    uint32_t nodeCount = 0u;
};

/// @brief Every body of water one frame draws.
struct WaterFrame
{
    std::vector<WaterBody> bodies;
    std::vector<WaterNode> nodes;
    /// One body's patches while they are picked; kept for its capacity.
    std::vector<WaterNode> scratch;
    /// How many bodies the scene holds, in view or not: while any does, their
    /// wave textures are kept for when they come back into view.
    uint32_t placed = 0u;

    void Clear()
    {
        bodies.clear();
        nodes.clear();
        scratch.clear();
        placed = 0u;
    }
};

} // namespace Assisi::Render
