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
#include <Assisi/Render/SkyProbeInputs.hpp>
#include <Assisi/Render/SpecularIbl.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

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

    /// One while a prefiltered environment answers the specular half, and zero
    /// otherwise. At zero mesh.frag takes the expression it had before there
    /// was an environment at all, so every provider without one draws exactly
    /// as it did.
    float specularEnvironment = 0.0f;
    /// The prefiltered cube's last mip, which is where roughness one is stored.
    float specularMaxLod = 0.0f;
    float specularPadding0 = 0.0f;
    float specularPadding1 = 0.0f;
};

// Each colour has to start its own sixteen bytes, which is what its padding is
// for. Gathering the two pads at the end would compile and pack to the same
// size while landing the second colour halfway into the first lane.
static_assert(sizeof(IndirectConstants) == 48 && offsetof(IndirectConstants, groundRadiance) == 16 &&
              offsetof(IndirectConstants, specularEnvironment) == 32,
              "The indirect lanes must stay three std140 vec4s, or every constant after them shifts.");

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

    /// @brief The mean radiance arriving at @p worldPosition along a GGX lobe
    /// of @p roughness about @p reflection — the split sum's environment half —
    /// or nothing, for a provider that has no environment to reflect.
    ///
    /// Nothing is not black. A provider that answers nothing leaves the shader
    /// drawing the indirect term as a single diffuse radiance, which is what
    /// the renderer drew before any provider had an environment; answering
    /// black would instead take the diffuse term's share away and replace it
    /// with no reflection.
    ///
    /// This is the reference the GPU's prefiltered cube approximates, computed
    /// by sampling, and far too slow to run per frame.
    ///
    /// @param reflection  Unit vector, world space.
    /// @param roughness   Perceptual roughness.
    [[nodiscard]] virtual std::optional<Assisi::Math::Color3> SpecularRadiance(const glm::vec3 & /*worldPosition*/,
                                                                               const glm::vec3 & /*reflection*/,
                                                                               float /*roughness*/) const
    {
        return std::nullopt;
    }
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

/// @brief The hemisphere's diffuse, and the sky itself as the thing a glossy
/// surface reflects.
///
/// The diffuse half is unchanged from HemisphereIndirect: a Lambertian surface
/// integrates over its whole hemisphere, and the two means already are that
/// integral for the directions that matter. The specular half is what the
/// hemisphere cannot answer — a metal reflects the sky in a direction, not an
/// average of it — and the GPU answers it from a cube baked out of the same sky
/// (SkyProbe), which this prefilters by sampling as the reference.
class SkyProbeIndirect final : public IndirectLighting
{
public:
    /// @param environment  The sky the probe was baked from, disks already gone.
    /// @param maxLod       The baked cube's last mip.
    SkyProbeIndirect(const Assisi::Math::Color3 &skyRadiance, const Assisi::Math::Color3 &groundRadiance,
                     const SkyProbeInputs &environment, float maxLod)
        : _hemisphere(skyRadiance, groundRadiance), _environment(environment),
        _maxLod(std::isfinite(maxLod) ? std::max(maxLod, 0.0f) : 0.0f)
    {
    }

    [[nodiscard]] Assisi::Math::Color3 Radiance(const glm::vec3 &worldPosition, const glm::vec3 &normal) const override
    {
        return _hemisphere.Radiance(worldPosition, normal);
    }

    [[nodiscard]] IndirectConstants ShaderConstants() const override
    {
        IndirectConstants constants = _hemisphere.ShaderConstants();
        constants.specularEnvironment = 1.0f;
        constants.specularMaxLod = _maxLod;
        return constants;
    }

    [[nodiscard]] std::optional<Assisi::Math::Color3> SpecularRadiance(const glm::vec3 & /*worldPosition*/,
                                                                       const glm::vec3 &reflection,
                                                                       float roughness) const override
    {
        const SkyProbeInputs &sky = _environment;
        const auto radiance = [&sky](const glm::vec3 &d) { return SkyRadiance(d, sky.sun, sky.moon, sky.settings); };
        return Assisi::Math::Color3(PrefilterGgx(radiance, reflection, roughness, kMaxPrefilterSampleCount));
    }

private:
    HemisphereIndirect _hemisphere;
    SkyProbeInputs _environment;
    float _maxLod;
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
/// **Both disks are excluded.** Each is a directional light, already counted once
/// as direct lighting, and integrating one here would light every shadowed
/// surface with the very body it is shadowed from. Everything else the model
/// produces is in: the scattering from both bodies, the aureole around a low sun,
/// the ground's bounce, and the night floor that keeps a moonless scene off pure
/// black — so a moonlit night has a moonlit ambient without anything asking for
/// one.
///
/// Dropping the moon's disk here is also what keeps the CPU free of its albedo
/// texture: the disk is the only term that texture multiplies, and this is the
/// only CPU path that evaluates the sky.
[[nodiscard]] inline SkyAmbient AmbientFromSky(const SkySun &sun, const SkyMoon &rawMoon,
                                               const SkySettings &rawSettings)
{
    SkySettings settings = Sanitized(rawSettings);
    settings.sunDiskIntensity = 0.0f;
    SkyMoon moon = Sanitized(rawMoon);
    moon.diskIntensity = 0.0f;

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

        sky += SkyRadiance(horizontal + glm::vec3(0.0f, cosZenith, 0.0f), sun, moon, settings);
        ground += SkyRadiance(horizontal - glm::vec3(0.0f, cosZenith, 0.0f), sun, moon, settings);
    }

    const float perSample = 1.0f / static_cast<float>(kAmbientSampleCount);
    return SkyAmbient{.sky = sky * perSample, .ground = ground * perSample};
}

/// @brief The same two means for a sky with no moon in it.
[[nodiscard]] inline SkyAmbient AmbientFromSky(const SkySun &sun, const SkySettings &rawSettings)
{
    return AmbientFromSky(sun, SkyMoon{}, rawSettings);
}

} // namespace Assisi::Render
