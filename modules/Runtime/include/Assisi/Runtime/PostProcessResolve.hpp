/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PostProcessResolve.hpp
/// @brief The scene's PostProcessVolumes blended into the settings one frame
/// is finished with, seen from where the camera is.

#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/PostProcessSettings.hpp>
#include <Assisi/Runtime/PostProcessVolume.hpp>

#include <cstdint>

namespace Assisi::Runtime
{

/// @brief What ResolvePostProcess found.
struct PostProcessResolution
{
    Render::PostProcessSettings settings;
    /// How many volumes cover the whole level. More than one is a mistake: the
    /// highest priority wins.
    uint32_t levelVolumes = 0u;
};

/// @brief How much a box volume counts with the camera @p distance metres
/// outside it: fully inside, nothing past its blend distance, and straight
/// between.
[[nodiscard]] float PostProcessVolumeWeight(float distance, float blendDistance);

/// @brief @p settings with @p volume's switched-on settings blended over them
/// by @p weight, from 0 for no change to 1 for the volume's own.
[[nodiscard]] Render::PostProcessSettings BlendPostProcessVolume(const Render::PostProcessSettings &settings,
                                                                 const PostProcessVolume &volume, float weight);

/// @brief The settings @p scene's volumes give a camera at @p camera: the
/// defaults, then each whole-level volume, then each box near enough to count,
/// lower priority first and, between boxes of equal priority, larger first.
[[nodiscard]] PostProcessResolution ResolvePostProcess(const ECS::Scene &scene, const glm::dvec3 &camera);

} // namespace Assisi::Runtime
