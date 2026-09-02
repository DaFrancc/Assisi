/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file IndirectLighting.hpp
/// @brief The indirect-lighting seam: what radiance reaches a surface from
/// everything that is not a light.
///
/// One question — "what indirect radiance arrives here, on a surface facing this
/// way?" — and several ways to answer it. A sky/ground gradient answers it from
/// two colours (@ref HemisphereIndirect). A baked answer reads a lightmap or a
/// probe; a hybrid one reads a probe and relights it from the lights that are
/// on right now; a fully dynamic one traces or voxelises. They differ in how the
/// answer is computed and how fresh it is, never in what is being asked, so
/// materials, shaders, assets and the rest of the renderer are identical across
/// all of them and a level can swap providers without invalidating content.
///
/// The GPU half of the seam is mesh.frag's IndirectRadiance(), which evaluates
/// @ref IndirectConstants per fragment. @ref EvaluateIndirect is that same
/// expression on the CPU, and the two must agree: a provider is written once and
/// answers both the shader and anything on this side that needs to know what a
/// point receives.

#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Sky.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Assisi::Render
{

/// @brief The indirect term a surface gets when no provider says otherwise.
///
/// Low, and worth leaving low: this one is unshadowed and unoccluded by
/// construction, so raising it flattens everything it touches.
inline constexpr float kDefaultAmbientIntensity = 0.03f;

/// @brief One frame's indirect answer, in the form the shader evaluates.
///
/// Field order, types and packing match the matching lanes of mesh.frag's
/// FrameConstants block. A provider that cannot say its answer in these two
/// lanes adds what it needs here and to IndirectRadiance() together — the two
/// halves are one thing, and a lane added to only one of them shifts every
/// offset after it.
///
/// Each radiance is a colour followed by its own padding rather than a Color4,
/// because the fourth lane is std140's alignment and not an alpha: a Color4 here
/// would say the shader had a coverage to read.
struct IndirectConstants
{
    /// Radiance reaching a surface facing straight up.
    Assisi::Math::Color3 skyRadiance{0.0f};
    float skyPadding = 0.0f;
    /// The same for one facing straight down.
    Assisi::Math::Color3 groundRadiance{0.0f};
    float groundPadding = 0.0f;
};

// Each colour has to start its own sixteen bytes, which is what its padding is
// for. Gathering the two pads at the end would compile and pack to the same
// size while landing the second colour halfway into the first lane.
static_assert(sizeof(IndirectConstants) == 32 && offsetof(IndirectConstants, groundRadiance) == 16,
              "The indirect lanes must stay two std140 vec4s, or every constant after them shifts.");

/// @brief The radiance a surface facing @p normal receives, given @p constants.
///
/// mesh.frag's IndirectRadiance() transcribes this. The normal's vertical
/// component alone selects between the two halves; the saturate is what stops a
/// normal that is not quite unit length from extrapolating past a hemisphere and
/// producing a negative radiance.
[[nodiscard]] inline Assisi::Math::Color3 EvaluateIndirect(const IndirectConstants &constants,
                                                           const glm::vec3 &normal)
{
    const float upward = std::clamp(normal.y * 0.5f + 0.5f, 0.0f, 1.0f);
    // Through the vectors: glm::mix on the colour type deduces its blend factor
    // from the colour and refuses to make a float of it.
    return glm::mix(glm::vec3(constants.groundRadiance), glm::vec3(constants.skyRadiance), upward);
}

/// @brief Abstract source of indirect radiance.
class IndirectLighting
{
public:
    virtual ~IndirectLighting() = default;

    /// @brief Radiance arriving at @p worldPosition on a surface facing
    /// @p normal, in linear RGB.
    ///
    /// A cosine-weighted mean radiance, which is what makes a caller's
    /// `Radiance() * albedo` the correct Lambertian response with no factor of
    /// pi anywhere: a Lambertian surface reflects irradiance/pi times its
    /// albedo, and the cosine-weighted mean IS the irradiance over pi.
    ///
    /// The position is unread by a provider whose answer is the same everywhere
    /// and load-bearing for every provider that stores an answer per place. It
    /// is in the question because leaving it out is what would force the
    /// interface open later.
    ///
    /// @param normal  Unit vector, world space; up is +Y.
    [[nodiscard]] virtual Assisi::Math::Color3 Radiance(const glm::vec3 &worldPosition,
                                                        const glm::vec3 &normal) const = 0;

    /// @brief The same answer as the shader's per-frame constants.
    ///
    /// EvaluateIndirect(ShaderConstants(), N) equals Radiance(p, N) for every
    /// normal, which is the whole agreement between the two halves of the seam.
    [[nodiscard]] virtual IndirectConstants ShaderConstants() const = 0;
};

/// @brief One radiance for every direction and every place.
///
/// The flat term the engine had before there was a sky, and still the right
/// answer for an interior, for a scene that authors its own ambient, and for the
/// model viewer, where a raised uniform term is how a mesh is visible without
/// anyone having to light it.
class UniformIndirect final : public IndirectLighting
{
public:
    UniformIndirect(const Assisi::Math::Color3 &color, float intensity)
        : _radiance(SanitizedSkyChannels(color * intensity, glm::vec3(0.0f)))
    {
    }

    [[nodiscard]] Assisi::Math::Color3 Radiance(const glm::vec3 & /*worldPosition*/,
                                                const glm::vec3 & /*normal*/) const override
    {
        return _radiance;
    }

    [[nodiscard]] IndirectConstants ShaderConstants() const override
    {
        return IndirectConstants{.skyRadiance = _radiance, .groundRadiance = _radiance};
    }

private:
    Assisi::Math::Color3 _radiance;
};

/// @brief Sky above, ground below, interpolated by which way the surface faces.
///
/// The cheapest answer that makes a shadowed surface read as lit by the world
/// rather than as a hole: what a shadow hides is the sun, and the sky is still
/// over it. Everything a surface receives that did not come straight from a
/// light comes from one of two directions here, which is wrong in the way a
/// gradient is wrong — no room, no bounce off the red wall beside it — and right
/// in the way that matters most for an outdoor scene, where the sky IS most of
/// the indirect light.
class HemisphereIndirect final : public IndirectLighting
{
public:
    HemisphereIndirect(const Assisi::Math::Color3 &skyRadiance, const Assisi::Math::Color3 &groundRadiance)
        : _sky(SanitizedSkyChannels(skyRadiance, glm::vec3(0.0f))),
        _ground(SanitizedSkyChannels(groundRadiance, glm::vec3(0.0f)))
    {
    }

    [[nodiscard]] Assisi::Math::Color3 Radiance(const glm::vec3 & /*worldPosition*/,
                                                const glm::vec3 &normal) const override
    {
        return EvaluateIndirect(ShaderConstants(), normal);
    }

    [[nodiscard]] IndirectConstants ShaderConstants() const override
    {
        return IndirectConstants{.skyRadiance = _sky, .groundRadiance = _ground};
    }

private:
    Assisi::Math::Color3 _sky;
    Assisi::Math::Color3 _ground;
};

/// @brief What the sky sends down, and what its ground half sends back up.
struct SkyAmbient
{
    Assisi::Math::Color3 sky{0.0f};
    Assisi::Math::Color3 ground{0.0f};
};

/// @brief How many directions each hemispherical mean is taken over.
///
/// A sky with no disk in it is smooth — one broad glow around the sun over a
/// gradient — so the mean converges early and more samples buy nothing visible.
/// The cost is this many SkyRadiance evaluations per hemisphere per frame, on
/// the CPU, which is microseconds.
inline constexpr uint32_t kAmbientSampleCount = 32;

/// @brief The golden angle, in radians. Successive multiples of it never land
/// near each other, which is what spreads the azimuths evenly without a lattice
/// that could line up with the sun.
inline constexpr float kGoldenAngle = 2.39996323f;

/// @brief The sky's two hemispherical means: what a surface facing straight up
/// receives from the sky, and what one facing straight down receives from the
/// ground half of the same model.
///
/// Cosine-weighted, so each is the hemisphere's irradiance over pi and multiplies
/// an albedo directly — see IndirectLighting::Radiance.
///
/// **The sun's disk is excluded.** It is the directional light, already counted
/// once as direct lighting, and integrating it here would light every shadowed
/// surface with the very sun it is shadowed from. Everything else the model
/// produces is in: the scattering, the aureole around a low sun, the ground's
/// bounce, and the night floor that keeps a moonless scene off pure black.
[[nodiscard]] inline SkyAmbient AmbientFromSky(const SkySun &sun, const SkySettings &rawSettings)
{
    SkySettings settings = Sanitized(rawSettings);
    settings.sunDiskIntensity = 0.0f;

    glm::vec3 sky{0.0f};
    glm::vec3 ground{0.0f};
    // Vectors while they are a running sum: what is accumulating is not a colour
    // until it is divided by the count.
    for (uint32_t i = 0; i < kAmbientSampleCount; ++i)
    {
        // A cosine-distributed elevation: the square root maps a uniform
        // fraction onto the angles whose density is the cosine, so the plain
        // mean over these samples is the cosine-weighted one and no weight has
        // to be carried or divided out.
        const float fraction = (static_cast<float>(i) + 0.5f) / static_cast<float>(kAmbientSampleCount);
        const float cosZenith = std::sqrt(1.0f - fraction);
        const float sinZenith = std::sqrt(fraction);
        const float azimuth = static_cast<float>(i) * kGoldenAngle;
        const glm::vec3 horizontal(std::cos(azimuth) * sinZenith, 0.0f, std::sin(azimuth) * sinZenith);

        sky += SkyRadiance(horizontal + glm::vec3(0.0f, cosZenith, 0.0f), sun, settings);
        ground += SkyRadiance(horizontal - glm::vec3(0.0f, cosZenith, 0.0f), sun, settings);
    }

    const float perSample = 1.0f / static_cast<float>(kAmbientSampleCount);
    return SkyAmbient{.sky = sky * perSample, .ground = ground * perSample};
}

} // namespace Assisi::Render
