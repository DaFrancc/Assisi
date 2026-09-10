/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/IndirectLighting.hpp>
#include <Assisi/Render/Sky.hpp>
#include <Assisi/Render/SkyProbeInputs.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

using namespace Assisi::Render;
using Assisi::Math::Color3;

namespace
{
/// Relative error allowed of a mirror's reflection against the sky it
/// reflects: the same direction reached through a normalise, and nothing else.
constexpr float kMirrorTolerance = 1e-4f;

/// A direction from an elevation above the horizon and an azimuth about +Y.
glm::vec3 Dir(float elevationDegrees, float azimuthDegrees)
{
    const float el = glm::radians(elevationDegrees);
    const float az = glm::radians(azimuthDegrees);
    return glm::vec3(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
}

/// Normals spread over the whole sphere, so a claim about the seam is not made
/// from the two poles alone.
std::vector<glm::vec3> Normals()
{
    std::vector<glm::vec3> normals;
    for (int32_t elevation = -90; elevation <= 90; elevation += 15)
    {
        for (int32_t azimuth = 0; azimuth < 360; azimuth += 45)
        {
            normals.push_back(Dir(static_cast<float>(elevation), static_cast<float>(azimuth)));
        }
    }
    return normals;
}

float Luminance(const Color3 &linear)
{
    return glm::dot(linear, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

bool AllFinite(const Color3 &v)
{
    return std::isfinite(v.r) && std::isfinite(v.g) && std::isfinite(v.b);
}

SkySun SunAt(float elevationDegrees)
{
    return SkySun{.directionToSun = Dir(elevationDegrees, 0.0f), .color = glm::vec3(1.0f), .intensity = 1.0f};
}
} // namespace

TEST_CASE("A uniform provider answers the same everywhere and in every direction")
{
    const UniformIndirect provider(Color3(0.4f, 0.6f, 1.0f), 0.5f);
    const Color3 expected(0.2f, 0.3f, 0.5f);

    for (const glm::vec3 &normal : Normals())
    {
        const Color3 here = provider.Radiance(glm::vec3(0.0f), normal);
        const Color3 elsewhere = provider.Radiance(glm::vec3(1000.0f, -20.0f, 7.0f), normal);
        CHECK(here.r == doctest::Approx(expected.r));
        CHECK(here.g == doctest::Approx(expected.g));
        CHECK(here.b == doctest::Approx(expected.b));
        CHECK(elsewhere.b == doctest::Approx(expected.b));
    }
}

TEST_CASE("A hemisphere provider gives the sky to a surface facing up and the ground to one facing down")
{
    const Color3 sky(0.20f, 0.35f, 0.70f);
    const Color3 ground(0.09f, 0.08f, 0.06f);
    const HemisphereIndirect provider(sky, ground);

    const Color3 up = provider.Radiance(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(up.b == doctest::Approx(sky.b));
    CHECK(up.r == doctest::Approx(sky.r));

    const Color3 down = provider.Radiance(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    CHECK(down.b == doctest::Approx(ground.b));
    CHECK(down.r == doctest::Approx(ground.r));

    // A wall sees half of each, which is the whole content of the gradient.
    const Color3 sideways = provider.Radiance(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(sideways.b == doctest::Approx(0.5f * (sky.b + ground.b)));

    // And the gradient is monotonic: tilting a surface skyward never darkens it.
    float previous = -1.0f;
    for (int32_t elevation = -90; elevation <= 90; elevation += 10)
    {
        const float luminance = Luminance(provider.Radiance(glm::vec3(0.0f), Dir(static_cast<float>(elevation), 0.0f)));
        CHECK(luminance >= previous);
        previous = luminance;
    }
}

TEST_CASE("Both halves of the seam agree: the CPU query and the shader's constants")
{
    // The invariant that keeps mesh.frag's IndirectRadiance() and this side one
    // thing rather than two. A provider that answered them differently would
    // light the editor's queries and the screen from different worlds.
    const UniformIndirect uniform(Color3(1.0f, 0.9f, 0.8f), 0.03f);
    const HemisphereIndirect hemisphere(Color3(0.2f, 0.35f, 0.7f), Color3(0.09f, 0.08f, 0.06f));
    const IndirectLighting *const providers[] = {&uniform, &hemisphere};

    for (const IndirectLighting *const provider : providers)
    {
        for (const glm::vec3 &normal : Normals())
        {
            const Color3 queried = provider->Radiance(glm::vec3(3.0f, 1.0f, -2.0f), normal);
            const Color3 shaded = EvaluateIndirect(provider->ShaderConstants(), normal);
            CHECK(queried.r == doctest::Approx(shaded.r));
            CHECK(queried.g == doctest::Approx(shaded.g));
            CHECK(queried.b == doctest::Approx(shaded.b));
        }
    }
}

TEST_CASE("A provider without an environment leaves the shader on the diffuse-only term")
{
    // What makes switching the probe off a baseline rather than an
    // approximation of one: these providers write zero into the specular lane,
    // and mesh.frag takes the expression it had before the lane existed.
    const UniformIndirect uniform(Color3(1.0f, 0.9f, 0.8f), 0.03f);
    const HemisphereIndirect hemisphere(Color3(0.2f, 0.35f, 0.7f), Color3(0.09f, 0.08f, 0.06f));
    const IndirectLighting *const providers[] = {&uniform, &hemisphere};

    for (const IndirectLighting *const provider : providers)
    {
        const IndirectConstants constants = provider->ShaderConstants();
        CHECK(constants.specularEnvironment == 0.0f);
        CHECK(constants.specularMaxLod == 0.0f);
        CHECK_FALSE(provider->SpecularRadiance(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0.5f).has_value());
    }
}

TEST_CASE("The sky probe keeps the hemisphere's diffuse and reflects the sky itself")
{
    const SkyProbeInputs environment = MakeSkyProbeInputs(SunAt(35.0f), SkyMoon{}, SkySettings{});
    const Color3 sky(0.2f, 0.35f, 0.7f);
    const Color3 ground(0.09f, 0.08f, 0.06f);
    const SkyProbeIndirect probe(sky, ground, environment, 4.0f);
    const HemisphereIndirect hemisphere(sky, ground);

    for (const glm::vec3 &normal : Normals())
    {
        const Color3 diffuse = probe.Radiance(glm::vec3(0.0f), normal);
        const Color3 expected = hemisphere.Radiance(glm::vec3(0.0f), normal);
        CHECK(diffuse.r == doctest::Approx(expected.r));
        CHECK(diffuse.b == doctest::Approx(expected.b));

        // A mirror reflects exactly the direction it faces, and a direction
        // of this sky, not an average of it.
        const std::optional<Color3> mirror = probe.SpecularRadiance(glm::vec3(0.0f), normal, 0.0f);
        REQUIRE(mirror.has_value());
        const glm::vec3 direct =
            SkyRadiance(normal, environment.sun, environment.moon, environment.settings);
        CHECK(mirror->r == doctest::Approx(direct.r).epsilon(kMirrorTolerance));
        CHECK(mirror->b == doctest::Approx(direct.b).epsilon(kMirrorTolerance));
    }

    const IndirectConstants constants = probe.ShaderConstants();
    CHECK(constants.specularEnvironment == 1.0f);
    CHECK(constants.specularMaxLod == doctest::Approx(4.0f));
    CHECK(constants.skyRadiance.b == doctest::Approx(sky.b));
    CHECK(constants.groundRadiance.r == doctest::Approx(ground.r));
}

TEST_CASE("The sky probe reflects the zenith and the horizon differently")
{
    // The whole reason for a probe over the hemisphere: a glossy surface facing
    // the horizon reflects the bright band there, and one facing up the deeper
    // zenith, where the hemisphere hands both the same average.
    const SkyProbeInputs environment = MakeSkyProbeInputs(SunAt(40.0f), SkyMoon{}, SkySettings{});
    const SkyProbeIndirect probe(Color3(0.2f), Color3(0.1f), environment, 4.0f);

    const Color3 zenith = *probe.SpecularRadiance(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 0.3f);
    const Color3 horizon = *probe.SpecularRadiance(glm::vec3(0.0f), Dir(3.0f, 180.0f), 0.3f);
    CHECK(Luminance(horizon) > Luminance(zenith));
    CHECK(zenith.b > zenith.r);
}

TEST_CASE("A normal that is not unit length cannot drive the term negative")
{
    const IndirectConstants constants{.skyRadiance = Color3(0.2f, 0.35f, 0.7f),
                                      .groundRadiance = Color3(0.09f, 0.08f, 0.06f)};

    for (const glm::vec3 &scaled : {glm::vec3(0.0f, 8.0f, 0.0f), glm::vec3(0.0f, -8.0f, 0.0f)})
    {
        const Color3 radiance = EvaluateIndirect(constants, scaled);
        CHECK(AllFinite(radiance));
        CHECK(radiance.r >= 0.0f);
        CHECK(radiance.g >= 0.0f);
        CHECK(radiance.b >= 0.0f);
    }
}

TEST_CASE("A garbage provider input becomes darkness rather than a NaN on every surface")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const UniformIndirect uniform(Color3(nan, -1.0f, 2.0f), 1.0f);
    CHECK(AllFinite(uniform.Radiance(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f))));

    const HemisphereIndirect hemisphere(Color3(nan), Color3(0.0f, -3.0f, nan));
    for (const glm::vec3 &normal : Normals())
    {
        const Color3 radiance = hemisphere.Radiance(glm::vec3(0.0f), normal);
        CHECK(AllFinite(radiance));
        CHECK(radiance.r >= 0.0f);
    }
}

TEST_CASE("A clear day lights a shadow with the sky rather than leaving it black")
{
    // The reason this issue exists: a sun shadow over the flat default reads as a
    // pit. What replaces it has to be brighter than that default, and blue,
    // because the light in a shadow outdoors is the sky's.
    const SkyAmbient ambient = AmbientFromSky(SunAt(60.0f), SkySettings{});

    CHECK(AllFinite(ambient.sky));
    CHECK(AllFinite(ambient.ground));
    CHECK(Luminance(ambient.sky) > kDefaultAmbientIntensity);
    CHECK(ambient.sky.b > ambient.sky.r);

    // The ground half is lit second-hand off a dark albedo, so it is dimmer than
    // the sky over it — which is what gives the gradient a direction at all.
    CHECK(Luminance(ambient.ground) < Luminance(ambient.sky));
    CHECK(Luminance(ambient.ground) > 0.0f);
}

TEST_CASE("The sun's disk does not light the world a second time")
{
    // The disk IS the directional light. Integrating it into the ambient term
    // would put the sun back onto every surface the sun is shadowed from, which
    // is the one thing this term must not do.
    // A big sun, because a half-degree one falls between the samples and would
    // let a term that does integrate the disk pass this unchanged — the same
    // reason the exclusion has to be explicit rather than incidental: a disk
    // wide enough to be sampled at all makes the ambient jump as the sun crosses
    // one sample and then the next.
    SkySettings noDisk;
    noDisk.sunSizeDegrees = kMaxSunSizeDegrees;
    noDisk.sunDiskIntensity = 0.0f;
    SkySettings brightDisk = noDisk;
    brightDisk.sunDiskIntensity = kMaxSunDiskIntensity;

    for (const float elevation : {5.0f, 30.0f, 85.0f})
    {
        const SkyAmbient without = AmbientFromSky(SunAt(elevation), noDisk);
        const SkyAmbient with = AmbientFromSky(SunAt(elevation), brightDisk);
        CHECK(with.sky.r == doctest::Approx(without.sky.r));
        CHECK(with.sky.g == doctest::Approx(without.sky.g));
        CHECK(with.sky.b == doctest::Approx(without.sky.b));
        CHECK(with.ground.b == doctest::Approx(without.ground.b));
    }
}

TEST_CASE("The moon's disk does not light the world twice either")
{
    // Same rule, second body — and one more consequence besides. The moon's disk
    // is the only term its albedo texture multiplies, and the CPU cannot sample a
    // texture; excluding the disk here is what keeps the CPU sky a complete
    // specification rather than one with a picture missing from it.
    SkySettings settings;
    settings.sunDiskIntensity = 0.0f;

    SkyMoon noDisk;
    noDisk.directionToMoon = Dir(50.0f, 120.0f);
    noDisk.color = glm::vec3(0.75f, 0.85f, 1.0f);
    noDisk.intensity = 0.02f;
    // Wide enough for the lattice to land inside, for the same reason the sun's
    // is: a half-degree disk falls between the samples and would let a term that
    // does integrate it pass unchanged.
    noDisk.sizeDegrees = kMaxSunSizeDegrees;
    noDisk.diskIntensity = 0.0f;

    SkyMoon brightDisk = noDisk;
    brightDisk.diskIntensity = kMaxSunDiskIntensity;

    for (const float elevation : {-20.0f, 5.0f, 60.0f})
    {
        const SkyAmbient without = AmbientFromSky(SunAt(elevation), noDisk, settings);
        const SkyAmbient with = AmbientFromSky(SunAt(elevation), brightDisk, settings);
        CHECK(with.sky.r == doctest::Approx(without.sky.r));
        CHECK(with.sky.b == doctest::Approx(without.sky.b));
        CHECK(with.ground.b == doctest::Approx(without.ground.b));
    }

    // What the moon DOES contribute is its scattering, and it has to survive:
    // a night with a moon up is brighter than one without, and that is where
    // moonlit ambient comes from.
    const SkyAmbient moonless = AmbientFromSky(SunAt(-20.0f), settings);
    const SkyAmbient moonlit = AmbientFromSky(SunAt(-20.0f), noDisk, settings);
    CHECK(Luminance(moonlit.sky) > Luminance(moonless.sky));
    CHECK(Luminance(moonlit.ground) > Luminance(moonless.ground));
}

TEST_CASE("An airless world has no sky to be lit by, and lit ground under it")
{
    SkySettings settings;
    settings.airThickness = 0.0f;
    settings.hazeScattering = glm::vec3(0.0f);
    settings.nightColor = glm::vec3(0.0f);
    settings.sunDiskIntensity = 0.0f;

    const SkyAmbient ambient = AmbientFromSky(SunAt(45.0f), settings);
    CHECK(Luminance(ambient.sky) == doctest::Approx(0.0f));
    CHECK(Luminance(ambient.ground) > 0.0f);
}

TEST_CASE("Night falls to the sky's own floor instead of to a hole or a NaN")
{
    const SkySettings settings;
    const SkyAmbient noon = AmbientFromSky(SunAt(80.0f), settings);
    const SkyAmbient night = AmbientFromSky(SunAt(-30.0f), settings);

    CHECK(AllFinite(night.sky));
    CHECK(AllFinite(night.ground));
    CHECK(night.sky.r >= 0.0f);
    CHECK(Luminance(night.sky) < Luminance(noon.sky));
    // The night colour is added unconditionally, so nothing is ever pure black.
    CHECK(Luminance(night.sky) > 0.0f);
}

TEST_CASE("A sky's ambient follows the world it describes")
{
    // The point of deriving the term rather than authoring it: change the air and
    // the light in the shadows changes with it, at every knob and with nothing
    // else edited.
    SkySettings snow;
    snow.groundColor = glm::vec3(0.85f);
    const SkyAmbient overSnow = AmbientFromSky(SunAt(30.0f), snow);
    const SkyAmbient overDirt = AmbientFromSky(SunAt(30.0f), SkySettings{});
    CHECK(Luminance(overSnow.ground) > Luminance(overDirt.ground));

    SkySettings dusty = SkySettings{};
    dusty.hazeScattering = glm::vec3(0.030f, 0.022f, 0.012f);
    const SkyAmbient overDust = AmbientFromSky(SunAt(30.0f), dusty);
    // Dust that absorbs blue takes the blue out of what the sky sends down, so
    // the shadows under it warm up rather than merely dimming.
    CHECK(overDust.sky.b / overDust.sky.r < overDirt.sky.b / overDirt.sky.r);
}
