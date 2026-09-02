/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/IndirectLighting.hpp>
#include <Assisi/Runtime/IndirectResolve.hpp>

using Assisi::Math::Color3;
using Assisi::Render::AmbientFromSky;
using Assisi::Render::EvaluateIndirect;
using Assisi::Render::kDefaultAmbientIntensity;
using Assisi::Render::SkyAmbient;
using Assisi::Render::SkySettings;
using Assisi::Render::SkySun;
using Assisi::Runtime::AmbientOverride;
using Assisi::Runtime::ResolveIndirect;
using Assisi::Runtime::SkyResolution;
using Assisi::Runtime::SkyStatus;

namespace
{
SkyResolution SunnyDay()
{
    return SkyResolution{.status = SkyStatus::Ready,
                         .sun = SkySun{.directionToSun = glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f)),
                                       .color = glm::vec3(1.0f),
                                       .intensity = 1.0f},
                         .settings = SkySettings{}};
}

float Luminance(const Color3 &linear)
{
    return glm::dot(linear, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

Color3 FacingUp(const Assisi::Render::IndirectConstants &constants)
{
    return EvaluateIndirect(constants, glm::vec3(0.0f, 1.0f, 0.0f));
}

Color3 FacingDown(const Assisi::Render::IndirectConstants &constants)
{
    return EvaluateIndirect(constants, glm::vec3(0.0f, -1.0f, 0.0f));
}
} // namespace

TEST_CASE("A scene with no sky keeps the flat term it had before there was one")
{
    for (const SkyStatus status : {SkyStatus::NoDirectionalLight, SkyStatus::NoSkybox})
    {
        const SkyResolution sky{.status = status, .sun = SkySun{}, .settings = SkySettings{}};
        const Color3 up = FacingUp(ResolveIndirect(sky, AmbientOverride{}));
        const Color3 down = FacingDown(ResolveIndirect(sky, AmbientOverride{}));

        CHECK(up.r == doctest::Approx(kDefaultAmbientIntensity));
        CHECK(up.g == doctest::Approx(kDefaultAmbientIntensity));
        CHECK(up.b == doctest::Approx(kDefaultAmbientIntensity));
        // Flat means flat: no gradient, whichever way a surface faces.
        CHECK(down.r == doctest::Approx(up.r));
        CHECK(down.b == doctest::Approx(up.b));
    }
}

TEST_CASE("Two suns are not a sky, so nothing under them is lit by one")
{
    const SkyResolution sky{
        .status = SkyStatus::MultipleDirectionalLights, .sun = SkySun{}, .settings = SkySettings{}};
    const Color3 up = FacingUp(ResolveIndirect(sky, AmbientOverride{}));
    CHECK(up.b == doctest::Approx(kDefaultAmbientIntensity));
}

TEST_CASE("A scene with a sky is lit by it, above and below")
{
    const SkyResolution sky = SunnyDay();
    const Assisi::Render::IndirectConstants constants = ResolveIndirect(sky, AmbientOverride{});
    const SkyAmbient expected = AmbientFromSky(sky.sun, sky.settings);

    const Color3 up = FacingUp(constants);
    CHECK(up.r == doctest::Approx(expected.sky.r));
    CHECK(up.b == doctest::Approx(expected.sky.b));

    const Color3 down = FacingDown(constants);
    CHECK(down.b == doctest::Approx(expected.ground.b));

    // The whole point: a shadowed surface under a clear sky is lit by it, and
    // more brightly than the flat default it replaces.
    CHECK(Luminance(up) > kDefaultAmbientIntensity);
    CHECK(up.b > up.r);
    // And it is a gradient rather than a constant, or the provider is doing
    // nothing a number could not.
    CHECK(Luminance(down) < Luminance(up));
}

TEST_CASE("A pinned ambient answers instead of the sky, not on top of it")
{
    // An interior has a sky over the building and is not lit by it. Whoever says
    // what the indirect term is has answered the question.
    const AmbientOverride indoors{.active = true, .color = Color3(0.5f, 0.4f, 0.35f), .intensity = 0.2f};
    const Color3 up = FacingUp(ResolveIndirect(SunnyDay(), indoors));
    const Color3 down = FacingDown(ResolveIndirect(SunnyDay(), indoors));

    CHECK(up.r == doctest::Approx(0.1f));
    CHECK(up.g == doctest::Approx(0.08f));
    CHECK(up.b == doctest::Approx(0.07f));
    CHECK(down.r == doctest::Approx(up.r));
}

TEST_CASE("An override that is not active leaves its colour unread")
{
    // The struct carries a colour whether or not it is pinned; a scene with a sky
    // must not quietly be lit by the one nobody turned on.
    const AmbientOverride idle{.active = false, .color = Color3(1.0f, 0.0f, 0.0f), .intensity = 5.0f};
    const Color3 up = FacingUp(ResolveIndirect(SunnyDay(), idle));
    CHECK(up.b > up.r);
}
