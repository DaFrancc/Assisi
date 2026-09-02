/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Render/IndirectLighting.hpp>
#include <Assisi/Runtime/SkyComponents.hpp>
#include <Assisi/Runtime/SkyResolve.hpp>

#include <cmath>

using namespace Assisi;
using Assisi::Runtime::DirectionalLight;
using Assisi::Runtime::ResolveSky;
using Assisi::Runtime::Skybox;
using Assisi::Runtime::SkyStatus;

namespace
{
/// An entity carrying a sun pointing straight down, i.e. a sun overhead.
///
/// Untinted by default so the colour it is given is the colour that comes out —
/// with tintedBySky on, the sun above the air is white, which is a different
/// thing to be testing.
ECS::Entity AddSun(ECS::Scene &scene, const glm::vec3 &travels = glm::vec3(0.f, -1.f, 0.f),
                   bool tintedBySky = false)
{
    const ECS::Entity entity = scene.Create();
    (void)scene.Add<DirectionalLight>(entity, DirectionalLight{.direction = travels,
                                                               .tintedBySky = tintedBySky,
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
    // Custom, because the knobs below are what this case is about: every other
    // preset answers outright and leaves them unread.
    skybox.preset = Assisi::Runtime::SkyPreset::Custom;
    skybox.exposure = 4.f;
    skybox.airThickness = 0.2f;
    (void)scene.Add<Skybox>(sun, skybox);

    const Runtime::SkyResolution resolved = ResolveSky(scene);
    REQUIRE(resolved.status == SkyStatus::Ready);

    // The light travels down, so the sun is up: the component stores where the
    // light goes and the sky is described by where the sun is.
    CHECK(resolved.sun.directionToSun.y == doctest::Approx(1.f));
    CHECK(glm::length(resolved.sun.directionToSun) == doctest::Approx(1.f));

    // With tintedBySky off, the sun's colour and intensity are the light's,
    // not the component's — one physical quantity lights the world, tints the sky
    // and colours the disk.
    CHECK(resolved.sun.color.g == doctest::Approx(0.9f));
    CHECK(resolved.sun.intensity == doctest::Approx(3.f));

    // The look is the component's own knobs.
    CHECK(resolved.settings.exposure == doctest::Approx(4.f));
    CHECK(resolved.settings.airThickness == doctest::Approx(0.2f));
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
    (void)scene.Add<DirectionalLight>(sun, DirectionalLight{.direction = glm::vec3(0.f, -1.f, 0.f),
                                                            .tintedBySky = true,
                                                            .color = glm::vec3(1.f),
                                                            .intensity = 1.f,
                                                            .castsShadows = true});
    (void)scene.Add<Skybox>(sun);

    // With tintedBySky on, the authored colour reaches nothing at all — not the light, and
    // not the sky either. A field the inspector greys out has to be inert
    // everywhere, or the grey is telling the author something untrue.
    const Runtime::SkyResolution resolved = ResolveSky(scene);
    REQUIRE(resolved.status == SkyStatus::Ready);
    CHECK(resolved.sun.color.r == doctest::Approx(1.f));
    CHECK(resolved.sun.color.b == doctest::Approx(1.f));
}

TEST_CASE("A greyed-out sun colour reaches nothing, and an authored one reaches the sky")
{
    const auto skyColorFor = [](bool tintedBySky)
                             {
                                 ECS::Scene scene;
                                 const ECS::Entity sun = scene.Create();
                                 (void)scene.Add<DirectionalLight>(sun, DirectionalLight{.direction = glm::vec3(0.f, -1.f, 0.f),
                                                                                         .tintedBySky = tintedBySky,
                                                                                         .color = glm::vec3(0.2f, 0.4f, 1.f),
                                                                                         .intensity = 1.f,
                                                                                         .castsShadows = true});
                                 (void)scene.Add<Skybox>(sun);
                                 const Runtime::SkyResolution r = ResolveSky(scene);
                                 REQUIRE(r.status == SkyStatus::Ready);
                                 return r.sun.color;
                             };

    // Greyed: the blue authored on the light is nowhere in the sky's input.
    const glm::vec3 fromSky = skyColorFor(true);
    CHECK(fromSky.r == doctest::Approx(1.f));
    CHECK(fromSky.g == doctest::Approx(1.f));
    CHECK(fromSky.b == doctest::Approx(1.f));

    // Editable: a blue sun scatters a blue sky, which is the whole reason to
    // author one.
    const glm::vec3 authored = skyColorFor(false);
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

    // An airless world leaves it alone entirely — no air AND nothing suspended
    // in it, since haze dims the beam just as the molecules do.
    Assisi::Render::SkySettings vacuum;
    vacuum.airThickness = 0.f;
    vacuum.hazeScattering = glm::vec3(0.f);
    const glm::vec3 unfiltered =
        LightingSystem::SunlightColor(white, glm::normalize(glm::vec3(0.f, 0.02f, 1.f)), &vacuum);
    CHECK(unfiltered.r == doctest::Approx(1.f));
    CHECK(unfiltered.b == doctest::Approx(1.f));
}

TEST_CASE("A sun's colour can be written as RGB or as a temperature")
{
    using Assisi::Runtime::AuthoredSunColor;
    using Assisi::Runtime::SunColorExpression;

    DirectionalLight light;
    light.colorExpression = SunColorExpression::Rgb;
    light.color = Assisi::Math::Color3(0.25f, 0.5f, 1.f);
    light.temperatureKelvin = 2800.f;

    // Under Rgb the temperature is not read, and under Temperature the colour is
    // not — which is what lets the inspector grey whichever is not in use.
    const glm::vec3 asRgb = AuthoredSunColor(light);
    CHECK(asRgb.r == doctest::Approx(0.25f));
    CHECK(asRgb.b == doctest::Approx(1.f));

    light.colorExpression = SunColorExpression::Temperature;
    const glm::vec3 asKelvin = AuthoredSunColor(light);
    CHECK(asKelvin.r > asKelvin.b);                      // 2800 K is tungsten-warm
    CHECK(asKelvin.b < asRgb.b);                          // and nothing of the blue RGB survived

    // Morning against afternoon: the pair of settings this exists for.
    light.temperatureKelvin = 7000.f;
    const glm::vec3 morning = AuthoredSunColor(light);
    light.temperatureKelvin = 3500.f;
    const glm::vec3 afternoon = AuthoredSunColor(light);
    CHECK(morning.b > afternoon.b);
    CHECK(afternoon.r / afternoon.b > morning.r / morning.b);
}

TEST_CASE("A temperature-authored sun reaches the sky and the world alike")
{
    ECS::Scene scene;
    const ECS::Entity sun = scene.Create();
    (void)scene.Add<DirectionalLight>(sun,
                                      DirectionalLight{.direction = glm::vec3(0.f, -1.f, 0.f),
                                                       .tintedBySky = false,
                                                       .colorExpression =
                                                           Assisi::Runtime::SunColorExpression::Temperature,
                                                       .color = Assisi::Math::Color3(1.f, 1.f, 1.f),
                                                       .temperatureKelvin = 3000.f,
                                                       .intensity = 1.f,
                                                       .castsShadows = true});
    (void)scene.Add<Skybox>(sun);

    // The sky scatters the sun it is given, so a warm sun makes a warm sky — the
    // temperature is the star's, not a grade applied to one half of the picture.
    const Runtime::SkyResolution resolved = ResolveSky(scene);
    REQUIRE(resolved.status == SkyStatus::Ready);
    CHECK(resolved.sun.color.r > resolved.sun.color.b);
    CHECK(resolved.sun.color.r == doctest::Approx(Assisi::Math::BlackbodyColor(3000.f).r));
}

TEST_CASE("Every preset is a complete, usable sky")
{
    using Assisi::Runtime::PresetSettings;
    using Assisi::Runtime::SkyPreset;

    const SkyPreset all[] = {SkyPreset::Clear,    SkyPreset::Arctic,   SkyPreset::Savanna, SkyPreset::Tropical,
                             SkyPreset::Alpine,   SkyPreset::Hazy, SkyPreset::Airless, SkyPreset::Custom};

    for (const SkyPreset preset : all)
    {
        const Assisi::Render::SkySettings s = PresetSettings(preset);
        // Sanitizing must be a no-op on every one of them: a preset that needed
        // clamping would be shipping a value no author could have typed.
        const Assisi::Render::SkySettings safe = Assisi::Render::Sanitized(s);
        CHECK(safe.airThickness == doctest::Approx(s.airThickness));
        CHECK(safe.exposure == doctest::Approx(s.exposure));
        CHECK(safe.hazeForwardness == doctest::Approx(s.hazeForwardness));
        CHECK(safe.hazeScattering.r == doctest::Approx(s.hazeScattering.r));
        CHECK(safe.sunDiskIntensity == doctest::Approx(s.sunDiskIntensity));
    }

    // Custom is the defaults, which is what makes departing from a preset start
    // somewhere sensible rather than at zero.
    const Assisi::Render::SkySettings custom = PresetSettings(SkyPreset::Custom);
    const Assisi::Render::SkySettings defaults;
    CHECK(custom.airThickness == doctest::Approx(defaults.airThickness));
    CHECK(custom.groundColor.r == doctest::Approx(defaults.groundColor.r));
}

TEST_CASE("The presets differ in the way their names claim")
{
    using Assisi::Runtime::PresetSettings;
    using Assisi::Runtime::SkyPreset;

    const auto clear = PresetSettings(SkyPreset::Clear);
    const auto arctic = PresetSettings(SkyPreset::Arctic);
    const auto savanna = PresetSettings(SkyPreset::Savanna);
    const auto tropical = PresetSettings(SkyPreset::Tropical);
    const auto alpine = PresetSettings(SkyPreset::Alpine);
    const auto hazy = PresetSettings(SkyPreset::Hazy);
    const auto airless = PresetSettings(SkyPreset::Airless);

    const auto haze = [](const Assisi::Render::SkySettings &s)
                      { return (s.hazeScattering.r + s.hazeScattering.g + s.hazeScattering.b) / 3.f; };

    // Clean cold air and thin mountain air both scatter less grey than a clear
    // day at sea level; humid and dusty air scatter more.
    CHECK(haze(arctic) < haze(clear));
    CHECK(haze(alpine) < haze(clear));
    CHECK(haze(tropical) > haze(clear));
    CHECK(haze(savanna) > haze(clear));

    // Dust absorbs blue, so the savanna's haze is warm where the tropics' is grey.
    CHECK(savanna.hazeScattering.r > savanna.hazeScattering.b);
    CHECK(tropical.hazeScattering.r == doctest::Approx(tropical.hazeScattering.b));

    // Less air overhead at altitude, more of it in the cold dense arctic.
    CHECK(alpine.airThickness < clear.airThickness);
    CHECK(arctic.airThickness > clear.airThickness);

    // Snow throws far more back up than any other ground here.
    CHECK(arctic.groundColor.b > 0.5f);
    CHECK(arctic.skyBounce > clear.skyBounce);

    // Hazy is the thickest air here: more suspended than the tropics, and a sun
    // softened rather than removed — the model cannot reach true overcast, so it
    // does not pretend to.
    CHECK(haze(hazy) > haze(tropical));
    CHECK(hazy.sunDiskIntensity > 0.f);
    CHECK(hazy.sunDiskIntensity < clear.sunDiskIntensity);
    CHECK(hazy.sunEdgeSoftness > clear.sunEdgeSoftness);

    // And airless is the limit: nothing to scatter in at all.
    CHECK(airless.airThickness == doctest::Approx(0.f));
    CHECK(haze(airless) == doctest::Approx(0.f));
}

TEST_CASE("The presets are told apart by the light they cast, not only by their numbers")
{
    using Assisi::Render::AmbientFromSky;
    using Assisi::Render::SkyAmbient;
    using Assisi::Runtime::PresetSettings;
    using Assisi::Runtime::SkyPreset;

    // Coefficients differing is not the same as a scene looking different. What
    // an author sees is this: the colour a shadowed surface is filled with, and
    // how far it is from the one the preset beside it gives.
    const float elevation = glm::radians(60.f);
    const Assisi::Render::SkySun sun{.directionToSun = glm::vec3(0.f, std::sin(elevation), std::cos(elevation)),
                                     .color = glm::vec3(1.f),
                                     .intensity = 3.f};

    const auto ambient = [&sun](SkyPreset preset) { return AmbientFromSky(sun, PresetSettings(preset)); };
    const auto luminance = [](const glm::vec3 &c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); };

    const SkyAmbient clear = ambient(SkyPreset::Clear);
    const SkyAmbient arctic = ambient(SkyPreset::Arctic);
    const SkyAmbient savanna = ambient(SkyPreset::Savanna);
    const SkyAmbient tropical = ambient(SkyPreset::Tropical);
    const SkyAmbient alpine = ambient(SkyPreset::Alpine);

    // The one that matters most for a place reading as hot or cold: the savanna's
    // dust scatters red where air scatters blue, so its shadows are filled with
    // WARM light — the sign of the comparison flips, rather than the amount
    // merely shifting.
    CHECK(savanna.sky.r > savanna.sky.b);
    CHECK(clear.sky.b > clear.sky.r);
    CHECK(alpine.sky.b > alpine.sky.r);
    CHECK(arctic.sky.b > arctic.sky.r);

    // Thin mountain air has the least of itself to scatter in, so it fills a
    // shadow with far less than a hazy or humid sky does. A spread this wide is
    // the difference between presets being a look and being a rounding error.
    CHECK(luminance(alpine.sky) * 5.f < luminance(tropical.sky));

    // Snow sends up more than the sky sends down; jungle sends up almost nothing.
    // Which way that ratio points is what decides whether a scene's shadows come
    // from above or below it.
    CHECK(luminance(arctic.ground) > luminance(arctic.sky));
    CHECK(luminance(tropical.ground) < luminance(tropical.sky));

    // And the ground each throws back keeps its own hue: snow is neutral, jungle
    // is green, dust is warm.
    CHECK(tropical.ground.g > tropical.ground.r);
    CHECK(tropical.ground.g > tropical.ground.b);
    CHECK(savanna.ground.r > savanna.ground.b);
}

TEST_CASE("A preset answers outright, and Custom is what reads the knobs")
{
    ECS::Scene scene;
    const ECS::Entity sun = AddSun(scene);
    Skybox skybox;
    skybox.preset = Assisi::Runtime::SkyPreset::Arctic;
    skybox.airThickness = 0.999f; // ignored while a preset is chosen
    (void)scene.Add<Skybox>(sun, skybox);

    const Runtime::SkyResolution presetSky = ResolveSky(scene);
    REQUIRE(presetSky.status == SkyStatus::Ready);
    CHECK(presetSky.settings.airThickness ==
          doctest::Approx(Assisi::Runtime::PresetSettings(Assisi::Runtime::SkyPreset::Arctic).airThickness));
    CHECK(presetSky.settings.airThickness != doctest::Approx(0.999f));

    // Switching to Custom is what makes those same stored knobs live again.
    Skybox *stored = scene.GetMut<Skybox>(sun);
    REQUIRE(stored != nullptr);
    stored->preset = Assisi::Runtime::SkyPreset::Custom;

    const Runtime::SkyResolution customSky = ResolveSky(scene);
    REQUIRE(customSky.status == SkyStatus::Ready);
    CHECK(customSky.settings.airThickness == doctest::Approx(0.999f));
}
