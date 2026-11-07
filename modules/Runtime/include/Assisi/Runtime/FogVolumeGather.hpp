/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FogVolumeGather.hpp
/// @brief The scene's fog volumes (FogBox, FogSphere, FogOutline), as one frame
/// draws them.

#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/FogVolume.hpp>

#include <cstdint>

namespace Assisi::Runtime
{

/// @brief What the frame is drawn from, for placing volumes on its screen.
struct FogVolumeView
{
    /// The lit pass's projection * view.
    glm::mat4 viewProjection{1.f};
    /// What the lit pass measures from.
    glm::dvec3 origin{0.0};
    uint32_t width = 0u;
    uint32_t height = 0u;
};

/// @brief What GatherFogVolumes could not draw.
struct FogVolumeGatherResult
{
    /// Volumes past kMaxFogVolumes, or whose outline would have taken the frame
    /// past kMaxFogOutlineEdges.
    uint32_t dropped = 0u;
};

/// @brief Fills @p frame with every fog volume on screen in @p scene: each as
/// the shader reads it, the outline edges, and every screen square's list.
///
/// A volume with no thickness or no size, wholly off screen, or with an outline
/// enclosing nothing, is left out.
FogVolumeGatherResult GatherFogVolumes(const ECS::Scene &scene, const FogVolumeView &view,
                                       Render::FogVolumeFrame &frame);

} // namespace Assisi::Runtime
