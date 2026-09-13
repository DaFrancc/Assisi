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
using Assisi::Runtime::SpecularProbe;

namespace
{
SkyResolution SunnyDay()
{
    SkyResolution sky;
    sky.status = SkyStatus::Ready;
    sky.sun = SkySun{.directionToSun = glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f)),
                     .color = glm::vec3(1.0f),
                     .intensity = 1.0f};
    return sky;
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
        SkyResolution sky;
        sky.status = status;
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
    SkyResolution sky;
    sky.status = SkyStatus::MultipleDirectionalLights;
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

TEST_CASE("A minimum ambient floors the night without touching the day")
{
    // A moonless night really is very nearly black, which is correct and is often
    // not playable. This is a floor under the sky's own answer, and a floor is
    // exactly what it has to be: an addition would brighten noon as well.
    SkyResolution night;
    night.status = SkyStatus::Ready;
    night.sun = SkySun{.directionToSun = glm::normalize(glm::vec3(0.2f, -0.6f, 0.1f)),
                       .color = glm::vec3(1.0f),
                       .intensity = 1.0f};

    const Color3 unlitUp = FacingUp(ResolveIndirect(night, AmbientOverride{}));
    const Color3 unlitDown = FacingDown(ResolveIndirect(night, AmbientOverride{}));

    night.minimumAmbient = glm::vec3(0.35f, 0.45f, 0.7f) * 0.05f;
    const Color3 flooredUp = FacingUp(ResolveIndirect(night, AmbientOverride{}));
    const Color3 flooredDown = FacingDown(ResolveIndirect(night, AmbientOverride{}));

    CHECK(Luminance(flooredUp) > Luminance(unlitUp));
    // Both halves, because "the world is at least partly lit" is about the world
    // and not about which way a surface happens to face.
    CHECK(Luminance(flooredDown) > Luminance(unlitDown));
    CHECK(flooredUp.b > flooredUp.r); // the floor's own hue reaches the surface

    // By day the floor is inert: the sky's term is orders above any sensible one,
    // so a level that sets a floor is unchanged at noon. This is what makes it
    // safe to leave on rather than something to schedule against the clock.
    SkyResolution day = SunnyDay();
    const Color3 beforeUp = FacingUp(ResolveIndirect(day, AmbientOverride{}));
    day.minimumAmbient = glm::vec3(0.35f, 0.45f, 0.7f) * 0.05f;
    const Color3 afterUp = FacingUp(ResolveIndirect(day, AmbientOverride{}));
    CHECK(afterUp.r == doctest::Approx(beforeUp.r));
    CHECK(afterUp.g == doctest::Approx(beforeUp.g));
    CHECK(afterUp.b == doctest::Approx(beforeUp.b));
}

TEST_CASE("A pinned ambient still outranks the floor")
{
    // Two answers to one question, and the author's wins. A floor that survived
    // an override would be a second answer arriving on top of the first.
    SkyResolution night;
    night.status = SkyStatus::Ready;
    night.sun = SkySun{.directionToSun = glm::normalize(glm::vec3(0.f, -1.f, 0.f)),
                       .color = glm::vec3(1.0f),
                       .intensity = 1.0f};
    night.minimumAmbient = glm::vec3(0.f, 1.0f, 0.f);

    const AmbientOverride pinned{.active = true, .color = Color3(1.0f, 0.0f, 0.0f), .intensity = 0.5f};
    const Color3 up = FacingUp(ResolveIndirect(night, pinned));
    CHECK(up.r > up.g);
}

TEST_CASE("A ready probe turns the sky's reflection on and leaves its diffuse alone")
{
    const SkyResolution sky = SunnyDay();
    const Assisi::Render::IndirectConstants without = ResolveIndirect(sky, AmbientOverride{});
    const Assisi::Render::IndirectConstants with =
        ResolveIndirect(sky, AmbientOverride{}, SpecularProbe{.ready = true, .maxLod = 4.0f});

    CHECK(without.specularEnvironment == 0.0f);
    CHECK(with.specularEnvironment == 1.0f);
    CHECK(with.specularMaxLod == doctest::Approx(4.0f));

    // The diffuse half is the hemisphere's either way: the probe adds the
    // reflection the hemisphere could not answer and changes nothing it could.
    CHECK(with.skyRadiance.r == doctest::Approx(without.skyRadiance.r));
    CHECK(with.skyRadiance.b == doctest::Approx(without.skyRadiance.b));
    CHECK(with.groundRadiance.g == doctest::Approx(without.groundRadiance.g));
}

TEST_CASE("A probe that has not baked yet reflects nothing")
{
    const Assisi::Render::IndirectConstants constants =
        ResolveIndirect(SunnyDay(), AmbientOverride{}, SpecularProbe{.ready = false, .maxLod = 4.0f});
    CHECK(constants.specularEnvironment == 0.0f);
    CHECK(constants.specularMaxLod == 0.0f);
}

TEST_CASE("A probe is the sky's, so nothing but a sky turns it on")
{
    // A ready probe left over from the last frame must not reach a scene that
    // pinned its ambient or lost its sky: it would reflect a world the diffuse
    // term says is not there.
    const SpecularProbe ready{.ready = true, .maxLod = 4.0f};

    const AmbientOverride indoors{.active = true, .color = Color3(0.5f), .intensity = 0.2f};
    CHECK(ResolveIndirect(SunnyDay(), indoors, ready).specularEnvironment == 0.0f);

    for (const SkyStatus status :
         {SkyStatus::NoDirectionalLight, SkyStatus::NoSkybox, SkyStatus::MultipleDirectionalLights})
    {
        SkyResolution sky = SunnyDay();
        sky.status = status;
        CHECK(ResolveIndirect(sky, AmbientOverride{}, ready).specularEnvironment == 0.0f);
    }
}
