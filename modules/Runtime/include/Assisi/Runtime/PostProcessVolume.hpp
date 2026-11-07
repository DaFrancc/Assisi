/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PostProcessVolume.hpp
/// @brief How a level, or a place in it, finishes its picture: exposure,
/// contrast, saturation, white balance and bloom.
///
/// One volume covering the whole level sets its look. A volume shaped as a box
/// changes the look inside it, a cave darker and warmer, fading in across its
/// blend distance as the camera nears, so nothing jumps at the edge. Each
/// setting has its own override switch, and a volume changes only the settings
/// it switches on, leaving the rest as the volumes under it set them.
///
/// The player's brightness, tone curve and dither, and whether bloom runs, go
/// on top, from their options.
///
/// Not replicable: a volume is level data, which every machine reads from the
/// same file.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/PostProcessSettings.hpp>

#include <cstdint>

namespace Assisi::Runtime
{

/// @brief Where a PostProcessVolume applies.
AENUM()
enum class PostProcessExtent : std::uint8_t
{
    Level, ///< Everywhere: the level's own look.
    Box,   ///< Inside a box centred on the entity, fading in across the blend distance.
};

/// @brief How a PostProcessVolume sets the exposure.
AENUM()
enum class ExposureMode : std::uint8_t
{
    Manual,    ///< A fixed number of stops.
    Automatic, ///< Follows the scene's brightness, as eyes adjust to it.
};

ACOMP()
struct PostProcessVolume
{
    AFIELD(radioBroadcast) PostProcessExtent extent = PostProcessExtent::Level;
    /// Width, height and depth, in metres, turned and scaled with the entity.
    /// A box with any of them zero or less covers nothing.
    AFIELD(radioListen = {source = extent, value = Box, behavior = vanish})
    glm::vec3 size{10.f, 4.f, 10.f};
    /// How far outside the box, in metres, the volume starts fading in.
    AFIELD(min = 0, radioListen = {source = extent, value = Box, behavior = vanish})
    float blendDistance = 2.f;
    /// Where volumes overlap, the higher priority applies on top. Between
    /// boxes of equal priority, the smaller one does.
    AFIELD() int32_t priority = 0;

    AFIELD(radioBroadcast) bool overrideExposure = false;
    AFIELD(radioBroadcast, radioListen = {source = overrideExposure, value = true, behavior = grey})
    ExposureMode exposureMode = ExposureMode::Automatic;
    /// Stops brighter than the scene's light as it is; negative is darker.
    AFIELD(min = -16.0, max = 16.0, radioListen = {source = exposureMode, value = Manual, behavior = vanish})
    float exposureStops = 0.f;
    /// The darkest and brightest the automatic exposure draws, in stops: it
    /// never brightens a dark scene by more than the maximum.
    AFIELD(min = -16.0, max = 16.0, radioListen = {source = exposureMode, value = Automatic, behavior = vanish})
    float minExposureStops = Render::kDefaultAutoExposureMinStops;
    AFIELD(min = -16.0, max = 16.0, radioListen = {source = exposureMode, value = Automatic, behavior = vanish})
    float maxExposureStops = Render::kDefaultAutoExposureMaxStops;
    /// Stops added to what the automatic exposure aims for: positive draws the
    /// scene brighter.
    AFIELD(min = -16.0, max = 16.0, radioListen = {source = exposureMode, value = Automatic, behavior = vanish})
    float exposureCompensation = 0.f;
    /// How fast the automatic exposure adjusts: each second it covers about
    /// all but one over e to this power of the way left.
    AFIELD(min = 0.0, max = 20.0, radioListen = {source = exposureMode, value = Automatic, behavior = vanish})
    float adaptationSpeed = Render::kDefaultExposureAdaptationRate;

    AFIELD(radioBroadcast) bool overrideContrast = false;
    AFIELD(min = 0.5, max = 2.5, radioListen = {source = overrideContrast, value = true, behavior = grey})
    float contrast = Render::kPunchyContrast;

    AFIELD(radioBroadcast) bool overrideSaturation = false;
    AFIELD(min = 0.0, max = 2.0, radioListen = {source = overrideSaturation, value = true, behavior = grey})
    float saturation = Render::kPunchySaturation;

    AFIELD(radioBroadcast) bool overrideWhiteBalance = false;
    /// The colour of light, in kelvin, that comes out white: set it to the
    /// light in the scene to make that light neutral. Higher warms the
    /// picture, lower cools it.
    AFIELD(min = 1700.0, max = 20000.0, radioListen = {source = overrideWhiteBalance, value = true, behavior = grey})
    float whiteBalanceKelvin = Render::kNeutralWhiteBalanceKelvin;
    /// From -1, toward green, to 1, toward magenta.
    AFIELD(min = -1.0, max = 1.0, radioListen = {source = overrideWhiteBalance, value = true, behavior = grey})
    float whiteBalanceTint = 0.f;

    AFIELD(radioBroadcast) bool overrideBloom = false;
    /// The share of the picture the glow around bright light replaces.
    AFIELD(min = 0.0, max = 0.5, radioListen = {source = overrideBloom, value = true, behavior = grey})
    float bloomIntensity = Render::kDefaultBloomIntensity;
    /// How widely the glow spreads, from tight around a light to a wide haze.
    AFIELD(min = 0.0, max = 0.95, radioListen = {source = overrideBloom, value = true, behavior = grey})
    float bloomScatter = Render::kDefaultBloomScatter;
};

} // namespace Assisi::Runtime
