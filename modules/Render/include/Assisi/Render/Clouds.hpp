/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Clouds.hpp
/// @brief The clouds: their knobs, the wind that moves them, and what the CPU
/// works out for the shaders that draw them.
///
/// The clouds themselves exist only on the GPU. sky/clouds.glsl marches them,
/// once per frame, into a small map of the sky above the horizon
/// (cloud_map.comp), which the sky pass and the reflection probe read; and into
/// a map of how much light they let through to the ground (cloud_shadow.comp),
/// which the lit pass reads. What is here is everything around that: the
/// settings and their limits, the sky map's layout, the wind's drift, the
/// constants the shaders take, and what clouds do to the sky's ambient light,
/// which is worked out on the CPU like the rest of the ambient.
///
/// **The camera stays below the clouds.** The sky map holds the whole of the
/// clouds along each direction, which is what lets one small map stand for them
/// at any screen size and for both eyes in VR; the price is that nothing can be
/// inside a cloud. A camera in or above the layer sees the clouds above it only.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Celestial.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/Sky.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace Assisi::Render
{

/// @name Limits and defaults
/// @{
/// @brief How much of the sky the clouds cover, from clear to overcast.
inline constexpr float kMinCloudCoverage = 0.0f;
inline constexpr float kMaxCloudCoverage = 1.0f;
inline constexpr float kDefaultCloudCoverage = 0.45f;

/// @brief The height of the clouds' flat bases, in metres above the world's
/// zero. Fair-weather cumulus form about a kilometre and a half up.
inline constexpr float kMinCloudHeight = -10000.0f;
inline constexpr float kMaxCloudHeight = 20000.0f;
inline constexpr float kDefaultCloudBaseHeight = 1500.0f;

/// @brief How tall the low layer is, base to top, in metres. Thinner than a
/// hundred metres the march's steps would skip whole clouds.
inline constexpr float kMinCloudThickness = 100.0f;
inline constexpr float kMaxCloudThickness = 10000.0f;
inline constexpr float kDefaultCloudThickness = 2000.0f;

/// @brief How quickly a cloud's interior hides what is behind it: the
/// extinction per metre where the cloud is densest. Cumulus measure between
/// 0.05 and 0.12; at the default, twenty-five metres of the densest cloud let
/// through a tenth of the light.
inline constexpr float kMinCloudDensity = 0.0f;
inline constexpr float kMaxCloudDensity = 1.0f;
inline constexpr float kDefaultCloudDensity = 0.1f;

/// @brief How heaped the low clouds are, from 0 (flat sheets of stratus) to 1
/// (towering cumulus). The sky mixes kinds around it.
inline constexpr float kMinCloudPuffiness = 0.0f;
inline constexpr float kMaxCloudPuffiness = 1.0f;
inline constexpr float kDefaultCloudPuffiness = 0.8f;

/// @brief How fast the clouds change shape: 0 never, 1 about as fast as real
/// cumulus, which form and fade over a quarter of an hour.
inline constexpr float kMinCloudChangeRate = 0.0f;
inline constexpr float kMaxCloudChangeRate = 10.0f;
inline constexpr float kDefaultCloudChangeRate = 1.0f;

/// @brief Wind speed in metres per second. A gale is about twenty.
inline constexpr float kMinCloudWindSpeed = 0.0f;
inline constexpr float kMaxCloudWindSpeed = 100.0f;
inline constexpr float kDefaultCloudWindSpeed = 8.0f;

/// @brief The compass bearing the clouds move toward: 0 north, 90 east.
inline constexpr float kDefaultCloudWindBearingDegrees = 90.0f;
/// @}

/// @brief The clouds as the renderer takes them.
struct CloudSettings
{
    float coverage = kDefaultCloudCoverage;
    float baseHeight = kDefaultCloudBaseHeight;
    float thickness = kDefaultCloudThickness;
    float density = kDefaultCloudDensity;
    float puffiness = kDefaultCloudPuffiness;
    float changeRate = kDefaultCloudChangeRate;
    float windSpeed = kDefaultCloudWindSpeed;
    float windBearingDegrees = kDefaultCloudWindBearingDegrees;
    /// Which sky of the kind the rest describes: each seed reads the noise from
    /// somewhere else, so the same settings give different clouds.
    uint32_t seed = 0;

    bool operator==(const CloudSettings &) const = default;
};

/// @brief @p settings with every field finite and in range.
[[nodiscard]] inline CloudSettings Sanitized(CloudSettings settings)
{
    const CloudSettings defaults;
    settings.coverage = ClampFiniteSky(settings.coverage, kMinCloudCoverage, kMaxCloudCoverage, defaults.coverage);
    settings.baseHeight =
        ClampFiniteSky(settings.baseHeight, kMinCloudHeight, kMaxCloudHeight, defaults.baseHeight);
    settings.thickness =
        ClampFiniteSky(settings.thickness, kMinCloudThickness, kMaxCloudThickness, defaults.thickness);
    settings.density = ClampFiniteSky(settings.density, kMinCloudDensity, kMaxCloudDensity, defaults.density);
    settings.puffiness =
        ClampFiniteSky(settings.puffiness, kMinCloudPuffiness, kMaxCloudPuffiness, defaults.puffiness);
    settings.changeRate =
        ClampFiniteSky(settings.changeRate, kMinCloudChangeRate, kMaxCloudChangeRate, defaults.changeRate);
    settings.windSpeed =
        ClampFiniteSky(settings.windSpeed, kMinCloudWindSpeed, kMaxCloudWindSpeed, defaults.windSpeed);
    settings.windBearingDegrees = std::isfinite(settings.windBearingDegrees)
                                      ? std::fmod(settings.windBearingDegrees, 360.0f)
                                      : defaults.windBearingDegrees;
    return settings;
}

/// @name The sky map's layout
///
/// Across is the bearing, all the way round; up is the elevation, from the
/// horizon to straight up. With the map four times as wide as it is tall, a
/// texel spans the same angle both ways, so the clouds near and overhead, which
/// fill the most of the screen, get as fine a texel as the ones on the horizon.
/// sky/cloud_map.glsl transcribes both.
/// @{
/// @brief Where a direction lands in the map. Directions below the horizon land
/// on its bottom row.
[[nodiscard]] inline glm::vec2 CloudMapUv(const glm::vec3 &direction)
{
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    constexpr float kHalfPi = 0.5f * std::numbers::pi_v<float>;
    const float bearingLength = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    const float bearing = bearingLength > kMinTangentLength ? std::atan2(direction.z, direction.x) : 0.0f;
    const float elevation = std::asin(std::clamp(direction.y, 0.0f, 1.0f));
    const float across = bearing / kTwoPi;
    return glm::vec2(across - std::floor(across), elevation / kHalfPi);
}

/// @brief The unit direction a point of the map stands for.
[[nodiscard]] inline glm::vec3 CloudMapDirection(const glm::vec2 &uv)
{
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    constexpr float kHalfPi = 0.5f * std::numbers::pi_v<float>;
    const float bearing = uv.x * kTwoPi;
    const float elevation = uv.y * kHalfPi;
    const float level = std::cos(elevation);
    return glm::vec3(std::cos(bearing) * level, std::sin(elevation), std::sin(bearing) * level);
}

/// @brief The map is this many times as wide as it is tall: it spans a whole
/// turn across and a quarter turn up.
inline constexpr uint32_t kCloudMapAspect = 4;
/// @}

/// @brief The wind's velocity, in metres per second along world x and z.
/// Double, because the drift it adds up to reaches tens of kilometres in a day
/// and the noise it moves through repeats every few.
[[nodiscard]] inline glm::dvec2 CloudWindVelocity(const CloudSettings &settings)
{
    const double bearing = glm::radians(static_cast<double>(settings.windBearingDegrees));
    const double speed = static_cast<double>(settings.windSpeed);
    // North is -z and east is +x, so a bearing turns from -z toward +x.
    return glm::dvec2(std::sin(bearing) * speed, -std::cos(bearing) * speed);
}

/// @brief The longest frame the clouds are moved across in one step, in
/// seconds. A stall, a breakpoint or a level load would otherwise throw them
/// kilometres in one frame.
inline constexpr double kMaxCloudDriftStep = 0.25;

/// @brief What time has done to the clouds so far, added up a little each
/// frame so that a change of speed changes how fast they go, not where they are.
struct CloudMotion
{
    /// How far the wind has carried the low clouds, in metres along x and z.
    glm::dvec2 drift{0.0};
    /// The same carry times the change rate, which moves the clouds' shapes
    /// against the weather that holds them.
    glm::dvec2 churn{0.0};
    /// Seconds of change, times the change rate: how far the clouds have
    /// billowed upward.
    double rise = 0.0;
};

/// @brief @p motion carried on by @p seconds of @p settings' wind and change.
[[nodiscard]] inline CloudMotion AdvanceClouds(const CloudMotion &motion, const CloudSettings &rawSettings,
                                               double seconds)
{
    const CloudSettings settings = Sanitized(rawSettings);
    const double step = std::clamp(seconds, 0.0, kMaxCloudDriftStep);
    const glm::dvec2 carried = CloudWindVelocity(settings) * step;
    const double change = static_cast<double>(settings.changeRate);
    return CloudMotion{.drift = motion.drift + carried,
                       .churn = motion.churn + carried * change,
                       .rise = motion.rise + step * change};
}

/// @name How the clouds change shape
///
/// The weather map rides the wind; the shape noise inside it lags a little and
/// rises, and the detail noise runs a little ahead and rises faster, so clouds
/// billow up, grow and fade as they travel. In metres per second of change, or
/// as shares of the wind's carry.
/// @{
inline constexpr double kCloudShapeLag = 0.1;
inline constexpr double kCloudDetailLead = 0.25;
inline constexpr double kCloudShapeRise = 1.0;
inline constexpr double kCloudDetailRise = 2.5;
/// @}

/// @name The noise
/// @{
/// @brief Texels along each edge of the shape and detail noise textures.
inline constexpr uint32_t kCloudShapeNoiseSize = 128;
inline constexpr uint32_t kCloudDetailNoiseSize = 64;

/// @brief How many metres each noise covers before it repeats. The shape noise
/// makes clouds about a quarter of its width across, and the detail noise wears
/// their edges at a hundred metres and finer. The weather map's: the air-mass
/// noise opens and closes stretches of sky about ten kilometres wide, the patch
/// noise groups clouds a few kilometres apart, and the kind noise mixes heaped
/// and flat cloud over tens of kilometres.
inline constexpr double kCloudShapeNoisePeriod = 9000.0;
inline constexpr double kCloudDetailNoisePeriod = 600.0;
inline constexpr double kCloudAirMassNoisePeriod = 48000.0;
inline constexpr double kCloudPatchNoisePeriod = 16000.0;
inline constexpr double kCloudKindNoisePeriod = 36000.0;

/// @brief The weather texture cloud_weather.comp bakes each frame: texels along
/// each side, and the metres it spans, which is twice the farthest the clouds
/// are drawn (sky/clouds.glsl's kMaxCloudDistance), so every cloud a ray meets
/// is on it. About six hundred metres a texel, still finer than the weather
/// map's patches, which are kilometres across.
inline constexpr uint32_t kCloudWeatherResolution = 512;
inline constexpr double kCloudWeatherWidth = 300000.0;

/// @brief Must match cloud_noise.comp's kShapeTexture and kDetailTexture.
enum class CloudNoiseKind : uint32_t
{
    Shape = 0,
    Detail = 1,
};

/// @brief Where a world position @p metres along one axis falls within a noise
/// repeating every @p period metres, in [0, 1). Taken in double so that the
/// float the shader adds it to keeps its precision however far out it is.
[[nodiscard]] inline float NoiseOffset(double metres, double period)
{
    const double repeats = metres / period;
    return static_cast<float>(repeats - std::floor(repeats));
}

/// @brief Each noise a seed shifts, in its own lanes of the seed's hash.
enum class CloudSeedLane : uint32_t
{
    Shape,
    Detail,
    AirMass,
    Patch,
    Kind,
    Count_,
};

/// @brief A fraction in [0, 1) that is the same for the same @p seed, @p lane
/// and @p axis every time, and unrelated between any two: SplitMix64.
[[nodiscard]] inline double SeedFraction(uint32_t seed, CloudSeedLane lane, uint32_t axis)
{
    // SplitMix64's increment and mixing multipliers, and a double's 53 bits.
    constexpr uint64_t kIncrement = 0x9E3779B97F4A7C15ull;
    constexpr uint64_t kMixA = 0xBF58476D1CE4E5B9ull;
    constexpr uint64_t kMixB = 0x94D049BB133111EBull;
    constexpr uint32_t kAxes = 3u;
    constexpr int kMantissaBits = 53;
    const uint64_t key = (static_cast<uint64_t>(seed) << 32u) |
                         (static_cast<uint64_t>(std::to_underlying(lane)) * kAxes + axis);
    uint64_t z = key + kIncrement;
    z = (z ^ (z >> 30u)) * kMixA;
    z = (z ^ (z >> 27u)) * kMixB;
    z ^= z >> 31u;
    return std::ldexp(static_cast<double>(z >> (64 - kMantissaBits)), -kMantissaBits);
}

/// @brief How far @p seed shifts one noise repeating every @p period metres,
/// in metres along x, up and z.
[[nodiscard]] inline glm::dvec3 SeedShift(uint32_t seed, CloudSeedLane lane, double period)
{
    return glm::dvec3(SeedFraction(seed, lane, 0u), SeedFraction(seed, lane, 1u), SeedFraction(seed, lane, 2u)) *
           period;
}
/// @}

/// @brief One slab of cloud in the constants. Mirrors sky/clouds.glsl's
/// CloudSlab; the member order is the layout.
struct CloudSlabConstants
{
    glm::vec4 layer{0.0f};    ///< x = base height, y = top height, z = coverage, w = extinction per metre
    glm::vec4 shape{0.0f};    ///< xy = the shape noise's offset across, z = up, w = repeats per metre
    glm::vec4 detail{0.0f};   ///< xy = the detail noise's offset across, z = up, w = repeats per metre
    glm::vec4 weatherA{0.0f}; ///< xy = the air-mass noise's offset across, zw = the patch noise's
    glm::vec4 weatherB{0.0f}; ///< xy = the kind noise's offset across, z = puffiness
};

ASSISI_GPU_LAYOUT(CloudSlabConstants);
ASSISI_GPU_FIRST_FIELD(CloudSlabConstants, layer);
ASSISI_GPU_FIELD_AFTER(CloudSlabConstants, shape, layer);
ASSISI_GPU_FIELD_AFTER(CloudSlabConstants, detail, shape);
ASSISI_GPU_FIELD_AFTER(CloudSlabConstants, weatherA, detail);
ASSISI_GPU_FIELD_AFTER(CloudSlabConstants, weatherB, weatherA);
ASSISI_GPU_NO_TAIL_PADDING(CloudSlabConstants, weatherB);

/// @brief Mirrors sky/clouds.glsl's CloudConstants; the member order is the
/// layout.
struct CloudConstants
{
    CloudSlabConstants low;
    glm::vec4 scales{0.0f};        ///< repeats per metre of x = air-mass, y = patch, z = kind noise; w = camera height
    glm::vec4 march{0.0f};         ///< x = steps through the layer, z = a map texel's width (radians)
    glm::vec4 weatherArea{0.0f};   ///< xy = the weather texture's centre across, camera-relative; z = 1 / its width; w = a texel's width
    glm::vec4 sunDirection{0.0f};  ///< xyz = unit direction to the sun, w = 1 to light the clouds by it
    glm::vec4 sunBeam{0.0f};       ///< rgb = the sunlight arriving at the clouds, times the sky's exposure
    glm::vec4 moonDirection{0.0f}; ///< xyz = unit direction to the moon, w = 1 to light the clouds by it
    glm::vec4 moonBeam{0.0f};      ///< rgb = the moonlight arriving at the clouds, times the sky's exposure
    glm::vec4 ambientSky{0.0f};    ///< rgb = the clear sky's light from above
    glm::vec4 ambientGround{0.0f}; ///< rgb = the light from below
};

ASSISI_GPU_LAYOUT(CloudConstants);
ASSISI_GPU_FIRST_FIELD(CloudConstants, low);
ASSISI_GPU_FIELD_AFTER(CloudConstants, scales, low);
ASSISI_GPU_FIELD_AFTER(CloudConstants, march, scales);
ASSISI_GPU_FIELD_AFTER(CloudConstants, weatherArea, march);
ASSISI_GPU_FIELD_AFTER(CloudConstants, sunDirection, weatherArea);
ASSISI_GPU_FIELD_AFTER(CloudConstants, sunBeam, sunDirection);
ASSISI_GPU_FIELD_AFTER(CloudConstants, moonDirection, sunBeam);
ASSISI_GPU_FIELD_AFTER(CloudConstants, moonBeam, moonDirection);
ASSISI_GPU_FIELD_AFTER(CloudConstants, ambientSky, moonBeam);
ASSISI_GPU_FIELD_AFTER(CloudConstants, ambientGround, ambientSky);
ASSISI_GPU_NO_TAIL_PADDING(CloudConstants, ambientGround);

/// @brief A body's light is marched toward only while it is at least this
/// share of the brighter body's, by luminance. Below it the body adds nothing
/// anyone could see, and marching toward it would double the clouds' cost for
/// that.
inline constexpr float kMinCloudBodyShare = 1e-3f;

/// @brief Rec. 709's luminance weights, the same the sky's tint uses.
inline constexpr glm::vec3 kCloudLuminanceWeights{0.2126f, 0.7152f, 0.0722f};

/// @brief What a body's light is once it has crossed the air to the clouds.
/// The air above the clouds is thinner than at the ground, but the clouds are
/// low against the air's height, so the ground's beam stands for it.
[[nodiscard]] inline glm::vec3 CloudBodyBeam(const glm::vec3 &toBody, const glm::vec3 &radiant,
                                             const SkySettings &settings, float tint)
{
    return radiant * TintedTransmittance(Transmittance(BeamExtinction(settings), SunAirMass(toBody.y)), tint);
}

/// @brief Whether a body whose beam has @p luminance lights the clouds, beside
/// one of @p brighter.
[[nodiscard]] inline bool CloudBodyLit(float luminance, float brighter)
{
    return brighter > 0.0f && luminance >= kMinCloudBodyShare * brighter;
}

/// @brief What one slab is, before it is placed around the camera.
struct CloudSlabLook
{
    float base = 0.0f;
    float thickness = 0.0f;
    float coverage = 0.0f;
    float density = 0.0f;
    float puffiness = 0.0f;
    double shapePeriod = 0.0;
    double detailPeriod = 0.0;
};

namespace Detail
{
/// Where a position falls within a noise, across in xy and up in z.
[[nodiscard]] inline glm::vec3 NoisePlace(const glm::dvec3 &position, double period)
{
    return glm::vec3(NoiseOffset(position.x, period), NoiseOffset(position.z, period),
                     NoiseOffset(position.y, period));
}
} // namespace Detail

/// @brief Place @p look around the camera at @p camera, moved by @p motion,
/// with the noise read where @p seed says.
[[nodiscard]] inline CloudSlabConstants PlaceCloudSlab(const CloudSlabLook &look, const glm::dvec3 &camera,
                                                       const CloudMotion &motion, uint32_t seed)
{
    // The camera's place among the clouds: its own position, less how far the
    // wind has carried them. The shape noise lags the weather map and the
    // detail noise runs ahead of it, and both rise; that is how the clouds
    // change shape as they travel.
    const glm::dvec2 churn = motion.churn;
    const glm::dvec3 weather(camera.x - motion.drift.x, 0.0, camera.z - motion.drift.y);
    // Rising noise is the noise read lower down, which is why rise subtracts.
    const glm::dvec3 shape =
        weather + glm::dvec3(churn.x * kCloudShapeLag, -motion.rise * kCloudShapeRise, churn.y * kCloudShapeLag);
    const glm::dvec3 detail =
        weather - glm::dvec3(churn.x * kCloudDetailLead, motion.rise * kCloudDetailRise, churn.y * kCloudDetailLead);

    const glm::vec3 shapePlace =
        Detail::NoisePlace(shape + SeedShift(seed, CloudSeedLane::Shape, look.shapePeriod), look.shapePeriod);
    const glm::vec3 detailPlace =
        Detail::NoisePlace(detail + SeedShift(seed, CloudSeedLane::Detail, look.detailPeriod), look.detailPeriod);
    const glm::vec3 airMass = Detail::NoisePlace(
        weather + SeedShift(seed, CloudSeedLane::AirMass, kCloudAirMassNoisePeriod), kCloudAirMassNoisePeriod);
    const glm::vec3 patch = Detail::NoisePlace(
        weather + SeedShift(seed, CloudSeedLane::Patch, kCloudPatchNoisePeriod), kCloudPatchNoisePeriod);
    const glm::vec3 kind = Detail::NoisePlace(
        weather + SeedShift(seed, CloudSeedLane::Kind, kCloudKindNoisePeriod), kCloudKindNoisePeriod);

    CloudSlabConstants slab;
    slab.layer = glm::vec4(look.base, look.base + look.thickness, look.coverage, look.density);
    slab.shape = glm::vec4(shapePlace, static_cast<float>(1.0 / look.shapePeriod));
    slab.detail = glm::vec4(detailPlace, static_cast<float>(1.0 / look.detailPeriod));
    slab.weatherA = glm::vec4(airMass.x, airMass.y, patch.x, patch.y);
    slab.weatherB = glm::vec4(kind.x, kind.y, look.puffiness, 0.0f);
    return slab;
}

/// @brief The constants block for @p settings around the camera at
/// @p cameraPosition, moved by @p motion; double, so the noise offsets hold
/// their precision. @p steps is the number of steps along each view ray through
/// the low layer and @p mapWidth the sky map's width in texels. The light comes
/// from LightClouds.
[[nodiscard]] inline CloudConstants PlaceClouds(const CloudSettings &rawSettings, const glm::dvec3 &cameraPosition,
                                                const CloudMotion &motion, uint32_t steps, uint32_t mapWidth)
{
    const CloudSettings settings = Sanitized(rawSettings);
    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

    CloudConstants constants;
    constants.low = PlaceCloudSlab(CloudSlabLook{.base = settings.baseHeight,
                                                 .thickness = settings.thickness,
                                                 .coverage = settings.coverage,
                                                 .density = settings.density,
                                                 .puffiness = settings.puffiness,
                                                 .shapePeriod = kCloudShapeNoisePeriod,
                                                 .detailPeriod = kCloudDetailNoisePeriod},
                                   cameraPosition, motion, settings.seed);
    constants.scales = glm::vec4(static_cast<float>(1.0 / kCloudAirMassNoisePeriod),
                                 static_cast<float>(1.0 / kCloudPatchNoisePeriod),
                                 static_cast<float>(1.0 / kCloudKindNoisePeriod), static_cast<float>(cameraPosition.y));
    constants.march = glm::vec4(static_cast<float>(steps), 0.0f, kTwoPi / static_cast<float>(std::max(mapWidth, 1u)),
                                0.0f);

    // Snapped to whole texels in the world, so a texel stands for the same
    // patch of sky as the camera moves and the clouds' edges do not crawl.
    const double texel = kCloudWeatherWidth / static_cast<double>(kCloudWeatherResolution);
    const glm::dvec2 camera(cameraPosition.x, cameraPosition.z);
    const glm::dvec2 centre = glm::floor(camera / texel + 0.5) * texel;
    constants.weatherArea = glm::vec4(glm::vec2(centre - camera), static_cast<float>(1.0 / kCloudWeatherWidth),
                                      static_cast<float>(texel));
    return constants;
}

/// @brief Fill @p constants' direct light: @p sun and @p moon through
/// @p rawSky. The light from the sky around them comes from AmbientLightClouds.
///
/// **In the sky's units.** The clouds scatter the beam the way the air does,
/// and the sky's exposure is what carries the air's scattered light to the
/// scene's scale, so it carries the clouds' too. Without it a cloud scatters a
/// tenth of what the air beside it does, and the sky's own light swamps the sun
/// on it: every cloud comes out the flat blue-grey of the ambient light.
inline void LightClouds(CloudConstants &constants, const SkySun &rawSun, const SkyMoon &rawMoon,
                        const SkySettings &rawSky)
{
    const SkySettings sky = Sanitized(rawSky);
    const SkySun sun = Sanitized(rawSun);
    const SkyMoon moon = Sanitized(rawMoon);

    const glm::vec3 sunBeam = CloudBodyBeam(sun.directionToSun, sun.color * sun.intensity, sky, 1.0f);
    const glm::vec3 moonBeam =
        CloudBodyBeam(moon.directionToMoon, moon.color * moon.intensity, sky, moon.atmosphericTint);
    const float sunLuminance = glm::dot(sunBeam, kCloudLuminanceWeights);
    const float moonLuminance = glm::dot(moonBeam, kCloudLuminanceWeights);
    const float brighter = std::max(sunLuminance, moonLuminance);

    constants.sunDirection = glm::vec4(sun.directionToSun, CloudBodyLit(sunLuminance, brighter) ? 1.0f : 0.0f);
    constants.sunBeam = glm::vec4(sunBeam * sky.exposure, 0.0f);
    constants.moonDirection = glm::vec4(moon.directionToMoon, CloudBodyLit(moonLuminance, brighter) ? 1.0f : 0.0f);
    constants.moonBeam = glm::vec4(moonBeam * sky.exposure, 0.0f);
}

/// @brief Fill @p constants' light from around: @p fromAbove and @p fromBelow,
/// the sky's own ambient light before the clouds change it (see
/// AmbientFromSky), which is what the clear sky above them and the ground
/// below send the clouds.
inline void AmbientLightClouds(CloudConstants &constants, const glm::vec3 &fromAbove, const glm::vec3 &fromBelow)
{
    constants.ambientSky = glm::vec4(fromAbove, 0.0f);
    constants.ambientGround = glm::vec4(fromBelow, 0.0f);
}

/// @name The light under the clouds
/// @{
/// @brief How much of the light above an overcast sky reaches the ground below
/// it, as soft light from the whole sky. A thick overcast lets through about a
/// quarter of a clear day's light.
inline constexpr float kOvercastTransmission = 0.25f;

/// @brief Optical depth a cloud's whole thickness reaches, as a share of what
/// solid cloud would: clouds are mostly thinner than their layer.
inline constexpr float kCloudTypicalFill = 0.3f;

/// @brief How much of the sky's light the clouds take over, from zero (clear)
/// to one (a closed overcast nothing gets through). Thin clouds cover the sky
/// without hiding it, so their coverage counts for less.
[[nodiscard]] inline float CloudCover(const CloudSettings &rawSettings)
{
    const CloudSettings settings = Sanitized(rawSettings);
    return settings.coverage * (1.0f - std::exp(-settings.density * settings.thickness * kCloudTypicalFill));
}

/// @brief The sun's and the moon's light on a level surface, over pi, as the
/// ambient light is measured.
[[nodiscard]] inline glm::vec3 DirectLightFromAbove(const SkySun &rawSun, const SkyMoon &rawMoon,
                                                    const SkySettings &rawSky)
{
    const SkySettings sky = Sanitized(rawSky);
    const SkySun sun = Sanitized(rawSun);
    const SkyMoon moon = Sanitized(rawMoon);
    const glm::vec3 sunBeam = CloudBodyBeam(sun.directionToSun, sun.color * sun.intensity, sky, 1.0f);
    const glm::vec3 moonBeam =
        CloudBodyBeam(moon.directionToMoon, moon.color * moon.intensity, sky, moon.atmosphericTint);
    constexpr float kInvPi = std::numbers::inv_pi_v<float>;
    return sunBeam * (std::max(sun.directionToSun.y, 0.0f) * kInvPi) +
           moonBeam * (std::max(moon.directionToMoon.y, 0.0f) * kInvPi);
}

/// @}

/// @name The shadow on the ground
///
/// A square of the clouds' base around the camera, where each texel holds how
/// much of the directional light gets through the clouds above it. A surface
/// follows the light up to the base and reads the texel there.
/// @{
/// @brief Texels along each side of the shadow map, and how many metres the
/// square spans: about thirty metres a texel, which is sharper than the edge of
/// any cloud's shadow on the ground.
inline constexpr uint32_t kCloudShadowResolution = 512;
inline constexpr float kCloudShadowWidth = 16000.0f;
/// @brief Steps through the layer toward the light for each texel.
inline constexpr uint32_t kCloudShadowSteps = 16;

/// @brief The lowest the light is taken to stand for the shadow, as the sine of
/// its elevation. A lower light would throw a shadow from clouds a hundred
/// kilometres off, outside the map, and lights almost nothing by then anyway.
inline constexpr float kMinCloudShadowLightHeight = 0.1f;

/// @brief The direction the shadow is cast along: @p toLight, raised to at
/// least kMinCloudShadowLightHeight.
[[nodiscard]] inline glm::vec3 CloudShadowLight(const glm::vec3 &toLight)
{
    if (toLight.y >= kMinCloudShadowLightHeight)
    {
        return toLight;
    }
    const float across = std::sqrt(toLight.x * toLight.x + toLight.z * toLight.z);
    const float level = std::sqrt(1.0f - kMinCloudShadowLightHeight * kMinCloudShadowLightHeight);
    if (across < kMinTangentLength)
    {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return glm::vec3(toLight.x / across * level, kMinCloudShadowLightHeight, toLight.z / across * level);
}

/// @brief The middle of the shadow map, across and relative to the camera:
/// where the light's line through the camera meets the clouds' base, so the
/// ground around the camera is what it covers. Snapped to whole texels in the
/// world, so a texel stands for the same patch of cloud as the camera moves and
/// the shadows' edges do not crawl.
[[nodiscard]] inline glm::vec2 CloudShadowCentre(const glm::dvec3 &cameraPosition, const glm::vec3 &shadowLight,
                                                 float baseHeight)
{
    const double rise = std::max(static_cast<double>(baseHeight) - cameraPosition.y, 0.0);
    const glm::dvec2 camera(cameraPosition.x, cameraPosition.z);
    const glm::dvec2 reach = glm::dvec2(shadowLight.x, shadowLight.z) * (rise / static_cast<double>(shadowLight.y));
    const double texel = static_cast<double>(kCloudShadowWidth) / static_cast<double>(kCloudShadowResolution);
    const glm::dvec2 snapped = glm::floor((camera + reach) / texel + 0.5) * texel;
    return glm::vec2(snapped - camera);
}

/// @brief Mirrors mesh/cloud_shadow.glsl's CloudShadowConstants: what the lit
/// pass needs to find a surface's texel in the shadow map. The member order is
/// the layout.
struct CloudShadowConstants
{
    /// xy = the map's centre across, relative to the camera; z = 1 / its width
    /// in metres; w = 1 while clouds cast.
    glm::vec4 area{0.0f};
    /// xyz = unit direction the shadow is cast along, toward the light; w = the
    /// clouds' base height.
    glm::vec4 light{0.0f};
    /// x = the light let through past the map's edge, y = the camera's height.
    glm::vec4 outside{0.0f};
};

ASSISI_GPU_LAYOUT(CloudShadowConstants);
ASSISI_GPU_FIRST_FIELD(CloudShadowConstants, area);
ASSISI_GPU_FIELD_AFTER(CloudShadowConstants, light, area);
ASSISI_GPU_FIELD_AFTER(CloudShadowConstants, outside, light);
ASSISI_GPU_NO_TAIL_PADDING(CloudShadowConstants, outside);

/// @brief Where the shadow lies this frame, for both the pass that draws it and
/// the lit pass that reads it.
struct CloudShadowPlacement
{
    /// The map's centre across, relative to the camera; see CloudShadowCentre.
    glm::vec2 centre{0.0f};
    /// The direction it is cast along; see CloudShadowLight.
    glm::vec3 light{0.0f, 1.0f, 0.0f};
};

/// @brief Place the shadow of @p settings under a light travelling along
/// @p lightTravel, with the camera at @p cameraPosition.
[[nodiscard]] inline CloudShadowPlacement PlaceCloudShadow(const CloudSettings &rawSettings,
                                                           const glm::dvec3 &cameraPosition,
                                                           const glm::vec3 &lightTravel)
{
    const CloudSettings settings = Sanitized(rawSettings);
    const glm::vec3 light = CloudShadowLight(-lightTravel);
    return CloudShadowPlacement{.centre = CloudShadowCentre(cameraPosition, light, settings.baseHeight),
                                .light = light};
}

/// @brief The lit pass's constants for a shadow placed by @p placement, with
/// the camera at @p cameraHeight.
[[nodiscard]] inline CloudShadowConstants MakeCloudShadowConstants(const CloudSettings &rawSettings,
                                                                   const CloudShadowPlacement &placement,
                                                                   float cameraHeight)
{
    const CloudSettings settings = Sanitized(rawSettings);
    return CloudShadowConstants{
        .area = glm::vec4(placement.centre, 1.0f / kCloudShadowWidth, 1.0f),
        .light = glm::vec4(placement.light, settings.baseHeight),
        // Past the map the clouds' shadows are too far off to make out one by
        // one, so the land there gets the light they let through on average.
        .outside = glm::vec4(1.0f - CloudCover(settings), cameraHeight, 0.0f, 0.0f)};
}
/// @}

/// @name The reflection probe
/// @{
/// @brief Whether clouds seen from @p currentAcross have moved across the sky
/// from where a probe baked from @p bakedAcross saw them, by more than
/// @p toleranceDegrees, for clouds @p heightAbove metres over the camera.
///
/// An "across" is the camera's position less the wind's drift: where the camera
/// stands among the clouds. Either the camera or the clouds moving changes it,
/// and a change of a given size moves low clouds across the sky further than
/// high ones. Zero tolerance compares exactly.
[[nodiscard]] inline bool CloudsMovedInSky(const glm::dvec2 &bakedAcross, const glm::dvec2 &currentAcross,
                                           float heightAbove, float toleranceDegrees)
{
    if (!(toleranceDegrees > 0.0f))
    {
        return bakedAcross != currentAcross;
    }
    const double moved = glm::length(currentAcross - bakedAcross);
    const double height = std::max(static_cast<double>(heightAbove), static_cast<double>(kMinCloudThickness));
    return std::atan2(moved, height) > glm::radians(static_cast<double>(toleranceDegrees));
}
/// @}

} // namespace Assisi::Render
