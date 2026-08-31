/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/SkyComponents.hpp>
#include <Assisi/Runtime/SkyResolve.hpp>

using namespace Assisi;
using Assisi::Runtime::DirectionalLight;
using Assisi::Runtime::ResolveSky;
using Assisi::Runtime::Skybox;
using Assisi::Runtime::SkyStatus;

namespace
{
/// An entity carrying a sun pointing straight down, i.e. a sun overhead.
///
/// Authored by default so the colour it is given is the colour that comes out —
/// under SunColorSource::Sky the sun above the air is white, which is a different
/// thing to be testing.
ECS::Entity AddSun(ECS::Scene &scene, const glm::vec3 &travels = glm::vec3(0.f, -1.f, 0.f),
                   Assisi::Runtime::SunColorSource source = Assisi::Runtime::SunColorSource::Authored)
{
    const ECS::Entity entity = scene.Create();
    (void)scene.Add<DirectionalLight>(entity, DirectionalLight{.direction = travels,
                                                               .colorSource = source,
                                                               .color = glm::vec3(1.f, 0.9f, 0.8f),
                                                               .intensity = 3.f,
                                                               .castsShadows = true});
    return entity;
}
} // namespace

TEST_CASE("A scene with no directional light has no sky")
{
    ECS::Scene scene;
    CHECK(ResolveSky(scene).status == SkyStatus::NoDirectionalLight);

    // Not even one that carries a Skybox: a sky is a lit atmosphere, and there is
    // nothing here to light it.
    const ECS::Entity orphan = scene.Create();
    (void)scene.Add<Skybox>(orphan);
    CHECK(ResolveSky(scene).status == SkyStatus::NoDirectionalLight);
}

TEST_CASE("A directional light on its own does not conjure an atmosphere")
{
    ECS::Scene scene;
    AddSun(scene);
    CHECK(ResolveSky(scene).status == SkyStatus::NoSkybox);
}

TEST_CASE("A sun carrying a Skybox is a sky")
{
    ECS::Scene scene;
    const ECS::Entity sun = AddSun(scene);
    Skybox skybox;
    skybox.intensity = 4.f;
    skybox.zenithOpticalDepth = 0.2f;
    (void)scene.Add<Skybox>(sun, skybox);

    const Runtime::SkyResolution resolved = ResolveSky(scene);
    REQUIRE(resolved.status == SkyStatus::Ready);

    // The light travels down, so the sun is up: the component stores where the
    // light goes and the sky is described by where the sun is.
    CHECK(resolved.sun.directionToSun.y == doctest::Approx(1.f));
    CHECK(glm::length(resolved.sun.directionToSun) == doctest::Approx(1.f));

    // With an authored colour, the sun's colour and intensity are the light's,
    // not the component's — one physical quantity lights the world, tints the sky
    // and colours the disk.
    CHECK(resolved.sun.color.g == doctest::Approx(0.9f));
    CHECK(resolved.sun.intensity == doctest::Approx(3.f));

    // The look is the component's.
    CHECK(resolved.settings.intensity == doctest::Approx(4.f));
    CHECK(resolved.settings.zenithOpticalDepth == doctest::Approx(0.2f));
}

TEST_CASE("The Skybox must be on the light, not merely in the scene")
{
    ECS::Scene scene;
    AddSun(scene);
    const ECS::Entity elsewhere = scene.Create();
    (void)scene.Add<Skybox>(elsewhere);

    // Otherwise "which light is this sky's sun" would be a guess, which is the
    // question the co-location requirement exists to answer.
    CHECK(ResolveSky(scene).status == SkyStatus::NoSkybox);
}

TEST_CASE("Two suns are unsupported, and say so rather than picking one")
{
    ECS::Scene scene;
    const ECS::Entity first = AddSun(scene, glm::vec3(0.f, -1.f, 0.f));
    (void)scene.Add<Skybox>(first);
    const ECS::Entity second = AddSun(scene, glm::vec3(1.f, -1.f, 0.f));

    CHECK(ResolveSky(scene).status == SkyStatus::MultipleDirectionalLights);

    // Including when both ask for a sky — the ambiguity is the problem, not the
    // components.
    (void)scene.Add<Skybox>(second);
    CHECK(ResolveSky(scene).status == SkyStatus::MultipleDirectionalLights);

    // Removing the second restores it: the rule is about how many there are now,
    // not about the scene having ever held two.
    scene.Remove<DirectionalLight>(second);
    CHECK(ResolveSky(scene).status == SkyStatus::Ready);
}

TEST_CASE("A degenerate light direction still resolves to a usable sun")
{
    ECS::Scene scene;
    const ECS::Entity sun = AddSun(scene, glm::vec3(0.f));
    (void)scene.Add<Skybox>(sun);

    const Runtime::SkyResolution resolved = ResolveSky(scene);
    REQUIRE(resolved.status == SkyStatus::Ready);
    // A zero direction is a fallback rather than a NaN that would reach the sky
    // shader and take every pixel of the frame with it.
    CHECK(glm::length(resolved.sun.directionToSun) == doctest::Approx(1.f));
}

TEST_CASE("A sun that casts no shadow still lights a sky")
{
    ECS::Scene scene;
    const ECS::Entity sun = scene.Create();
    (void)scene.Add<DirectionalLight>(sun, DirectionalLight{.direction = glm::vec3(0.f, -1.f, 0.f),
                                                            .color = glm::vec3(1.f),
                                                            .intensity = 1.f,
                                                            .castsShadows = false});
    (void)scene.Add<Skybox>(sun);

    // castsShadows decides whether the light gets cascades, which is a separate
    // question from whether there is daylight to scatter.
    CHECK(ResolveSky(scene).status == SkyStatus::Ready);
}

TEST_CASE("A sun tinted by its sky is lit by what reaches the ground")
{
    // The toggle is on DirectionalLight, and what it needs is a Skybox on the
    // same entity — the atmosphere doing the tinting.
    ECS::Scene scene;
    const ECS::Entity sun = scene.Create();
    (void)scene.Add<DirectionalLight>(sun,
                                      DirectionalLight{.direction = glm::vec3(0.f, -1.f, 0.f),
                                                       .colorSource = Assisi::Runtime::SunColorSource::Sky,
                                                       .color = glm::vec3(1.f),
                                                       .intensity = 1.f,
                                                       .castsShadows = true});
    (void)scene.Add<Skybox>(sun);

    // Under Sky the authored colour reaches nothing at all — not the light, and
    // not the sky either. A field the inspector greys out has to be inert
    // everywhere, or the grey is telling the author something untrue.
    const Runtime::SkyResolution resolved = ResolveSky(scene);
    REQUIRE(resolved.status == SkyStatus::Ready);
    CHECK(resolved.sun.color.r == doctest::Approx(1.f));
    CHECK(resolved.sun.color.b == doctest::Approx(1.f));
}

TEST_CASE("A greyed-out sun colour reaches nothing, and an authored one reaches the sky")
{
    const auto skyColorFor = [](Assisi::Runtime::SunColorSource source)
                             {
                                 ECS::Scene scene;
                                 const ECS::Entity sun = scene.Create();
                                 (void)scene.Add<DirectionalLight>(sun, DirectionalLight{.direction = glm::vec3(0.f, -1.f, 0.f),
                                                                                         .colorSource = source,
                                                                                         .color = glm::vec3(0.2f, 0.4f, 1.f),
                                                                                         .intensity = 1.f,
                                                                                         .castsShadows = true});
                                 (void)scene.Add<Skybox>(sun);
                                 const Runtime::SkyResolution r = ResolveSky(scene);
                                 REQUIRE(r.status == SkyStatus::Ready);
                                 return r.sun.color;
                             };

    // Greyed: the blue authored on the light is nowhere in the sky's input.
    const glm::vec3 fromSky = skyColorFor(Assisi::Runtime::SunColorSource::Sky);
    CHECK(fromSky.r == doctest::Approx(1.f));
    CHECK(fromSky.g == doctest::Approx(1.f));
    CHECK(fromSky.b == doctest::Approx(1.f));

    // Editable: a blue sun scatters a blue sky, which is the whole reason to
    // author one.
    const glm::vec3 authored = skyColorFor(Assisi::Runtime::SunColorSource::Authored);
    CHECK(authored.r == doctest::Approx(0.2f));
    CHECK(authored.b == doctest::Approx(1.f));
}

TEST_CASE("SunlightColor is what the atmosphere leaves of the light")
{
    using Assisi::Runtime::LightingSystem;
    const Assisi::Render::SkySettings air;
    const glm::vec3 white{1.f, 1.f, 1.f};

    // No atmosphere on the entity, or the light opted out: authored colour, whole.
    CHECK(LightingSystem::SunlightColor(white, glm::vec3(0.f, 1.f, 0.f), nullptr).r == doctest::Approx(1.f));
    CHECK(LightingSystem::SunlightColor(white, glm::vec3(0.f, 1.f, 0.f), nullptr).b == doctest::Approx(1.f));

    // Overhead, the air barely touches it — placing a Skybox must not visibly
    // darken a midday scene.
    const glm::vec3 noon = LightingSystem::SunlightColor(white, glm::vec3(0.f, 1.f, 0.f), &air);
    CHECK(noon.r == doctest::Approx(1.f).epsilon(0.05));
    CHECK(noon.b == doctest::Approx(1.f).epsilon(0.1));

    // Low, the world goes oranger AND dimmer, which is the whole feature.
    const glm::vec3 low =
        LightingSystem::SunlightColor(white, glm::normalize(glm::vec3(0.f, 0.02f, 1.f)), &air);
    CHECK(low.r > low.g);
    CHECK(low.g > low.b);
    CHECK(low.r < noon.r);

    // The authored colour is NOT read when the sky is supplying one. That is what
    // makes greying the field in the inspector honest rather than misleading.
    const glm::vec3 blue{0.2f, 0.4f, 1.f};
    const glm::vec3 fromSky = LightingSystem::SunlightColor(blue, glm::vec3(0.f, 1.f, 0.f), &air);
    CHECK(fromSky.r == doctest::Approx(noon.r));
    CHECK(fromSky.b == doctest::Approx(noon.b));

    // And it IS read when nothing is supplying one.
    CHECK(LightingSystem::SunlightColor(blue, glm::vec3(0.f, 1.f, 0.f), nullptr).r == doctest::Approx(blue.r));

    // An airless world leaves it alone entirely.
    Assisi::Render::SkySettings vacuum;
    vacuum.zenithOpticalDepth = 0.f;
    const glm::vec3 unfiltered =
        LightingSystem::SunlightColor(white, glm::normalize(glm::vec3(0.f, 0.02f, 1.f)), &vacuum);
    CHECK(unfiltered.r == doctest::Approx(1.f));
    CHECK(unfiltered.b == doctest::Approx(1.f));
}
