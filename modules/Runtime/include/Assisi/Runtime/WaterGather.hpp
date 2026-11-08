/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WaterGather.hpp
/// @brief The scene's Oceans and Lakes, as one frame draws them.

#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/WaterFrame.hpp>
#include <Assisi/Runtime/WaterComponents.hpp>
#include <Assisi/Runtime/WindComponents.hpp>

namespace Assisi::Runtime
{

/// @brief What the frame is drawn from.
struct WaterGatherView
{
    glm::dvec3 camera{0.0};
    /// The frustum, measured from the camera; null keeps every patch in range.
    const Render::Frustum *frustum = nullptr;
    /// Nothing further than this is drawn, in metres.
    float drawDistance = 0.0f;
};

/// @brief An Ocean's waves under @p wind.
[[nodiscard]] Render::WaterWaves OceanWaves(const Ocean &ocean, const Wind &wind);

/// @brief A Lake's waves, @p wind blowing over a rectangle @p extent metres
/// wide whose x axis points along @p axisX in the world: the fetch is the
/// rectangle's width along the wind.
[[nodiscard]] Render::WaterWaves LakeWaves(const Lake &lake, const Wind &wind, glm::vec2 extent, glm::vec2 axisX);

/// @brief Fills @p frame with every Ocean and Lake @p view sees, and the
/// patches that draw each, under the scene's wind. A body none of whose
/// patches is in view is left out.
void GatherWater(const ECS::Scene &scene, const WaterGatherView &view, Render::WaterFrame &frame);

} // namespace Assisi::Runtime
