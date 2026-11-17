/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/PostProcessResolve.hpp>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Assisi::Runtime
{
namespace
{

// A box whose entity scales it to less than this many cubic metres has nothing
// inside it, and its frame cannot be undone.
constexpr float kMinBoxVolume = 1.0e-9f;

// Kelvin over a million: colour temperatures blend evenly in these, where a
// blend in kelvin would spend most of its way near the cooler end.
constexpr float kMicroReciprocal = 1.0e6f;

/// @brief A volume near enough to count, and where it goes in the order.
struct Applied
{
    const PostProcessVolume *volume = nullptr;
    float weight = 0.0f;
    /// For boxes, so the smaller of two at one priority goes on top; infinite
    /// for the whole level, which goes under every box.
    float size = 0.0f;
    int32_t priority = 0;
    bool wholeLevel = false;
};

// Whether @p a goes under @p b.
bool AppliesBefore(const Applied &a, const Applied &b)
{
    if (a.wholeLevel != b.wholeLevel)
    {
        return a.wholeLevel;
    }
    if (a.priority != b.priority)
    {
        return a.priority < b.priority;
    }
    return a.size > b.size;
}

// How far, in metres, the camera at the origin of @p model's space is outside
// the box of @p halfExtents that @p model places; zero inside.
float DistanceOutside(const glm::mat4 &model, const glm::vec3 &halfExtents)
{
    const glm::vec3 local(glm::inverse(model) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec3 nearest = glm::clamp(local, -halfExtents, halfExtents);
    return glm::length(glm::vec3(model * glm::vec4(nearest, 1.0f)));
}

float Mix(float from, float to, float weight)
{
    return from + (to - from) * weight;
}

} // namespace

float PostProcessVolumeWeight(float distance, float blendDistance)
{
    if (!(distance > 0.0f))
    {
        return 1.0f;
    }
    if (!(blendDistance > 0.0f))
    {
        return 0.0f;
    }
    return std::clamp(1.0f - distance / blendDistance, 0.0f, 1.0f);
}

Render::PostProcessSettings BlendPostProcessVolume(const Render::PostProcessSettings &settings,
                                                   const PostProcessVolume &volume, float weight)
{
    Render::PostProcessSettings blended = settings;
    if (volume.overrideExposure)
    {
        Render::ExposureSettings &exposure = blended.exposure;
        if (volume.exposureMode == ExposureMode::Manual)
        {
            exposure.manualStops = Mix(exposure.manualStops, volume.exposureStops, weight);
            exposure.automatic = Mix(exposure.automatic, 0.0f, weight);
        }
        else
        {
            exposure.automatic = Mix(exposure.automatic, 1.0f, weight);
            exposure.minStops = Mix(exposure.minStops, volume.minExposureStops, weight);
            exposure.maxStops = Mix(exposure.maxStops, volume.maxExposureStops, weight);
            exposure.compensationStops = Mix(exposure.compensationStops, volume.exposureCompensation, weight);
            exposure.adaptationRate = Mix(exposure.adaptationRate, volume.adaptationSpeed, weight);
        }
    }
    if (volume.overrideContrast)
    {
        blended.contrast = Mix(blended.contrast, volume.contrast, weight);
    }
    if (volume.overrideSaturation)
    {
        blended.saturation = Mix(blended.saturation, volume.saturation, weight);
    }
    if (volume.overrideWhiteBalance)
    {
        const float from = kMicroReciprocal / std::max(blended.whiteBalanceKelvin, Render::kMinWhiteBalanceKelvin);
        const float to = kMicroReciprocal / std::max(volume.whiteBalanceKelvin, Render::kMinWhiteBalanceKelvin);
        blended.whiteBalanceKelvin = kMicroReciprocal / Mix(from, to, weight);
        blended.whiteBalanceTint = Mix(blended.whiteBalanceTint, volume.whiteBalanceTint, weight);
    }
    if (volume.overrideBloom)
    {
        blended.bloomIntensity = Mix(blended.bloomIntensity, volume.bloomIntensity, weight);
        blended.bloomScatter = Mix(blended.bloomScatter, volume.bloomScatter, weight);
    }
    return Render::Sanitized(blended);
}

PostProcessResolution ResolvePostProcess(const ECS::Scene &scene, const glm::dvec3 &camera)
{
    std::vector<Applied> applied;
    PostProcessResolution resolution;
    for (auto [entity, world, volume] : scene.Query<ECS::WorldMatrix, PostProcessVolume>())
    {
        if (volume.extent == PostProcessExtent::Level)
        {
            ++resolution.levelVolumes;
            applied.push_back(Applied{.volume = &volume,
                                      .weight = 1.0f,
                                      .size = std::numeric_limits<float>::infinity(),
                                      .priority = volume.priority,
                                      .wholeLevel = true});
            continue;
        }
        // Measured from the camera, so the box's frame stays small and exact
        // wherever in the world both are.
        const glm::mat4 model = ECS::MatrixFrom(world, camera);
        const glm::vec3 half = 0.5f * glm::max(volume.size, glm::vec3(0.0f));
        const float size = 8.0f * half.x * half.y * half.z * std::abs(glm::determinant(glm::mat3(model)));
        if (!(size > kMinBoxVolume))
        {
            continue;
        }
        const float weight = PostProcessVolumeWeight(DistanceOutside(model, half), volume.blendDistance);
        if (weight > 0.0f)
        {
            applied.push_back(
                Applied{.volume = &volume, .weight = weight, .size = size, .priority = volume.priority});
        }
    }
    std::stable_sort(applied.begin(), applied.end(), AppliesBefore);
    for (const Applied &entry : applied)
    {
        resolution.settings = BlendPostProcessVolume(resolution.settings, *entry.volume, entry.weight);
    }
    return resolution;
}

} // namespace Assisi::Runtime
