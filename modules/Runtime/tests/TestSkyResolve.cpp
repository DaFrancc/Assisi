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
#include <Assisi/Runtime/TimeOfDay.hpp>

#include <cmath>
#include <cstdint>

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

namespace
{
using Assisi::Runtime::LightingBody;
using Assisi::Runtime::Moon;
using Assisi::Runtime::Sun;
using Assisi::Runtime::TimeOfDay;

/// A sun on the clock: the light, the atmosphere it is seen through, and the
/// place on the planet its aim comes from.
ECS::Entity AddClockedSun(ECS::Scene &scene, float latitudeDegrees = 45.f, bool tintedBySky = true)
{
    const ECS::Entity entity = AddSun(scene, glm::vec3(0.f, -1.f, 0.f), tintedBySky);
    (void)scene.Add<Skybox>(entity);
    (void)scene.Add<Sun>(entity, Sun{.latitudeDegrees = latitudeDegrees});
    return entity;
}

TimeOfDay ClockAt(double hour, double dayOfYear = 0.0, float yearLengthDays = 12.f)
{
    TimeOfDay clock;
    clock.hour = hour;
    clock.dayOfYear = dayOfYear;
    clock.yearLengthDays = yearLengthDays;
    return clock;
}
} // namespace

TEST_CASE("A Sun takes the aim from the clock, and without one the authored aim stands")
{
    ECS::Scene scene;
    const glm::vec3 authored = glm::normalize(glm::vec3(0.3f, -0.7f, 0.2f));
    const ECS::Entity entity = AddSun(scene, authored, false);
    (void)scene.Add<Skybox>(entity);

    // Bitwise, because an author who aimed a light by hand expects the direction
    // they typed rather than one that has been through a rotation and back.
    const Runtime::SkyResolution authoredSky = ResolveSky(scene);
    CHECK(authoredSky.sun.directionToSun == -authored);
    CHECK(authoredSky.light.direction == authored);
    CHECK(authoredSky.light.body == LightingBody::Sun);

    (void)scene.Add<Sun>(entity, Sun{.latitudeDegrees = 45.f});
    (void)scene.Add<TimeOfDay>(entity, ClockAt(6.0));

    // Six in the morning on an equinox is due east, which the authored aim was
    // nowhere near.
    const Runtime::SkyResolution clocked = ResolveSky(scene);
    CHECK(clocked.sun.directionToSun.x == doctest::Approx(1.f).epsilon(1e-4));
    CHECK(clocked.sun.directionToSun.y == doctest::Approx(0.f).epsilon(1e-4));
}

TEST_CASE("A sun exactly on the horizon lights nothing, and so does the moon beside it")
{
    // The exact claim, made where it can be made exactly: a horizontal authored
    // aim puts the sun's vertical component at a true zero, and BOTH ramps are
    // zero there. That is what lets the slot change hands with nothing visible
    // happening — as against a rule keyed to which body is brighter, which would
    // swap precisely where the two are EQUAL and non-zero, around two percent of
    // noon, in both tinting modes.
    for (const bool tinted : {true, false})
    {
        CAPTURE(tinted);
        ECS::Scene scene;
        const ECS::Entity entity = AddSun(scene, glm::vec3(-1.f, 0.f, 0.f), tinted);
        (void)scene.Add<Skybox>(entity);

        const Runtime::SkyResolution sky = ResolveSky(scene);
        REQUIRE(sky.sun.directionToSun.y == 0.f);
        CHECK(sky.light.intensity == 0.f);
        CHECK(sky.light.body == LightingBody::None);
        CHECK_FALSE(sky.light.castsShadows);
    }
}

TEST_CASE("The sun and the moon never light at once, and the slot changes hands smoothly")
{
    for (const bool tinted : {true, false})
    {
        CAPTURE(tinted);
        ECS::Scene scene;
        const ECS::Entity entity = AddClockedSun(scene, 45.f, tinted);
        (void)scene.Add<Moon>(entity);
        (void)scene.Add<TimeOfDay>(entity, ClockAt(0.0, 3.0));

        const float authored = scene.Get<DirectionalLight>(entity)->intensity;

        LightingBody previous = LightingBody::None;
        float previousIntensity = 0.f;
        bool havePrevious = false;
        int32_t changes = 0;

        for (int32_t minute = 0; minute < 24 * 60; ++minute)
        {
            TimeOfDay *clock = scene.GetMut<TimeOfDay>(entity);
            REQUIRE(clock != nullptr);
            clock->hour = 24.0 * static_cast<double>(minute) / (24.0 * 60.0);

            const Runtime::SkyResolution sky = ResolveSky(scene);
            REQUIRE(sky.light.intensity >= 0.f);
            REQUIRE(std::isfinite(sky.light.intensity));

            // The gate, stated exactly: above the horizon the moon lights
            // nothing, below it the sun does. Never both, at any hour, so which
            // of the two branches is tested first decides nothing.
            if (sky.sun.directionToSun.y >= 0.f)
            {
                REQUIRE(sky.light.body != LightingBody::Moon);
            }
            if (sky.sun.directionToSun.y <= 0.f)
            {
                REQUIRE(sky.light.body != LightingBody::Sun);
            }

            if (havePrevious && sky.light.body != previous)
            {
                ++changes;
                // Both sides of the change are dark. Not exactly zero, because a
                // minute of sampling straddles the crossing rather than landing
                // on it — but under a hundredth of full, which is the claim that
                // matters and is two orders below where a brightness rule would
                // have put it.
                CHECK(sky.light.intensity < authored * 0.01f);
                CHECK(previousIntensity < authored * 0.01f);
            }
            previous = sky.light.body;
            previousIntensity = sky.light.intensity;
            havePrevious = true;
        }

        // Sunrise and sunset at least. More when the moon sets partway through
        // the night and leaves nothing lighting until dawn — a real state, and
        // the one a moonless night is: the scene falls to the sky's own floor
        // rather than to black, and pays for no shadow pass while it lasts.
        CHECK(changes >= 2);
    }
}

TEST_CASE("The midnight sun never gives the slot up")
{
    // Above the Arctic Circle at midsummer the sun does not set, so the night
    // gate never opens and the moon never lights however high it climbs. A steady
    // state with no timer and no transition in it.
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene, 67.f);
    (void)scene.Add<Moon>(entity);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(0.0, 3.0));

    for (int32_t minute = 0; minute < 24 * 60; minute += 5)
    {
        CAPTURE(minute);
        scene.GetMut<TimeOfDay>(entity)->hour = 24.0 * static_cast<double>(minute) / (24.0 * 60.0);
        const Runtime::SkyResolution sky = ResolveSky(scene);
        REQUIRE(sky.sun.directionToSun.y > 0.f);
        CHECK(sky.light.body == LightingBody::Sun);
        CHECK(sky.daylightHours == doctest::Approx(24.f));
    }
}

TEST_CASE("A daytime moon lights nothing, and is still in the sky")
{
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene);
    (void)scene.Add<Moon>(entity, Moon{.phaseAtEpoch = 0.25f});
    (void)scene.Add<TimeOfDay>(entity, ClockAt(15.0));

    const Runtime::SkyResolution sky = ResolveSky(scene);
    REQUIRE(sky.sun.directionToSun.y > 0.f);
    REQUIRE(sky.moon.directionToMoon.y > 0.f);

    // The sun holds the slot while it is up — not because it is checked first,
    // but because the night gate is shut. There is no configuration in which both
    // are lit, so the order of the two branches decides nothing.
    CHECK(sky.light.body == LightingBody::Sun);
    CHECK(sky.light.intensity > 0.f);

    // Still drawn, still scattering, and carrying a real phase — not quite a
    // clean half, because fifteen hours of a twenty-nine day month have already
    // gone past the quarter the epoch put it at. That drift is the phase being
    // geometry rather than a stored value.
    CHECK(sky.moon.diskIntensity > 0.f);
    CHECK(sky.moonLitFraction > 0.4f);
    CHECK(sky.moonLitFraction < 0.7f);
}

TEST_CASE("Moonlight keeps the moon's own colour, and reddens on top of it")
{
    // Moon::color is a field nothing greys out, so it has to be read. The sun's
    // authored colour is discarded under tintedBySky — the sky IS the colour
    // there, and the inspector says so — but nothing makes that claim about the
    // moon, and dropping it left the moon reddening from white, which is why a
    // low moon looked like a small sunset rather than a warm moon.
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene, 45.f, /*tintedBySky=*/true);
    Moon moon;
    moon.phaseAtEpoch = 0.5f;
    // Unmistakably green, so nothing else in the sky could have produced it.
    moon.color = Assisi::Math::Color3(0.2f, 1.0f, 0.3f);
    // The physics, full strength: this case is about the reddening existing at
    // all, and the tint that softens it has a case of its own below.
    moon.atmosphericTint = 1.f;
    (void)scene.Add<Moon>(entity, moon);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(0.0));

    const Runtime::SkyResolution sky = ResolveSky(scene);
    REQUIRE(sky.light.body == LightingBody::Moon);
    CHECK(sky.light.color.g > sky.light.color.r);
    CHECK(sky.light.color.g > sky.light.color.b);

    // And the air still reddens it: high in the sky it keeps more of its blue
    // than it does near the horizon, exactly as the sun does over the same path.
    float highBlue = 0.f;
    float lowBlue = 1.f;
    for (int32_t minute = 0; minute < 24 * 60; minute += 5)
    {
        scene.GetMut<TimeOfDay>(entity)->hour = 24.0 * static_cast<double>(minute) / (24.0 * 60.0);
        const Runtime::SkyResolution sample = ResolveSky(scene);
        if (sample.light.body != LightingBody::Moon || sample.light.color.g <= 0.f)
        {
            continue;
        }
        const float blueness = sample.light.color.b / sample.light.color.g;
        if (sample.moon.directionToMoon.y > 0.5f)
        {
            highBlue = std::max(highBlue, blueness);
        }
        if (sample.moon.directionToMoon.y > 0.f && sample.moon.directionToMoon.y < 0.1f)
        {
            lowBlue = std::min(lowBlue, blueness);
        }
    }
    REQUIRE(highBlue > 0.f);
    REQUIRE(lowBlue < 1.f);
    // A harvest moon: the same long path that reddens a low sun reddens this.
    CHECK(lowBlue < highBlue);
}

TEST_CASE("The moon's atmospheric tint reaches the world as well as the disk")
{
    // The world and the thing lighting it have to agree. If the disk were
    // softened and the light were not, a moon that looked its own colour would
    // still be casting sunset light on the ground.
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene, 45.f, /*tintedBySky=*/true);
    Moon moon;
    moon.phaseAtEpoch = 0.5f;
    moon.atmosphericTint = 1.f;
    (void)scene.Add<Moon>(entity, moon);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(0.0));

    // Find an hour where the moon is genuinely low, which is where the tint does
    // anything at all.
    bool found = false;
    for (int32_t minute = 0; minute < 24 * 60 && !found; minute += 2)
    {
        scene.GetMut<TimeOfDay>(entity)->hour = 24.0 * static_cast<double>(minute) / (24.0 * 60.0);
        const Runtime::SkyResolution sample = ResolveSky(scene);
        found = sample.light.body == LightingBody::Moon && sample.moon.directionToMoon.y > 0.f &&
                sample.moon.directionToMoon.y < 0.08f;
    }
    REQUIRE(found);

    const Runtime::SkyResolution physical = ResolveSky(scene);
    scene.GetMut<Moon>(entity)->atmosphericTint = 0.f;
    const Runtime::SkyResolution softened = ResolveSky(scene);

    REQUIRE(physical.light.body == LightingBody::Moon);
    REQUIRE(softened.light.body == LightingBody::Moon);

    // Less reddening in what lights the ground...
    CHECK(softened.light.color.b / softened.light.color.r > physical.light.color.b / physical.light.color.r);
    // ...and essentially no change in how much light there is, so turning the
    // knob is not turning the night up.
    //
    // Near rather than exact, and the gap is not slop: TintedTransmittance holds
    // luminance exactly, which TestSky asserts to the bit, but what lands here is
    // that transmittance times the moon's own colour — and the moon's colour is
    // not neutral, so the luminance of the product moves a little as the hue
    // under it does. A few percent, against a hue swing of tens of percent.
    const auto luminance = [](const glm::vec3 &c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); };
    CHECK(luminance(softened.light.color) == doctest::Approx(luminance(physical.light.color)).epsilon(0.1));
    // The intensity is untouched outright — the knob never reaches it.
    CHECK(softened.light.intensity == physical.light.intensity);
}

TEST_CASE("A new moon lights nothing even when it is up and the sun is down")
{
    // The phase is in the light, not only on the disk. Without it a new moon
    // would light the ground as brightly as a full one.
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(0.0));

    Moon newMoon;
    newMoon.phaseAtEpoch = 0.f;
    Moon fullMoon;
    fullMoon.phaseAtEpoch = 0.5f;

    (void)scene.Add<Moon>(entity, fullMoon);
    const Runtime::SkyResolution full = ResolveSky(scene);
    REQUIRE(full.moon.directionToMoon.y > 0.f);
    REQUIRE(full.sun.directionToSun.y < 0.f);
    REQUIRE(full.light.body == LightingBody::Moon);

    *scene.GetMut<Moon>(entity) = newMoon;
    const Runtime::SkyResolution none = ResolveSky(scene);
    CHECK(none.moonLitFraction < 0.01f);
    CHECK(none.light.intensity < full.light.intensity * 0.01f);
}

TEST_CASE("The sky's own sun is unramped at every hour, including under the horizon")
{
    // The ramps are about what lights the WORLD. Applying them to the sky would
    // cut the sunset off at the moment the sun touched the horizon, which is when
    // a sunset is only starting — the twilight extension carries the beam
    // eighteen degrees further down.
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(12.0));

    const DirectionalLight *light = scene.Get<DirectionalLight>(entity);
    REQUIRE(light != nullptr);
    const float authored = light->intensity;

    for (int32_t hour = 0; hour < 24; ++hour)
    {
        CAPTURE(hour);
        scene.GetMut<TimeOfDay>(entity)->hour = static_cast<double>(hour);
        const Runtime::SkyResolution sky = ResolveSky(scene);
        CHECK(sky.sun.intensity == doctest::Approx(authored));
    }
}

TEST_CASE("Winter light is warmer and dimmer than summer light with no field changed but the day")
{
    // Seasonal colour is not a knob and must never become one: a winter sun is
    // redder because it is LOWER, and the transmittance already computes exactly
    // that from the elevation the declination produced. A field for it would
    // multiply the same reddening in twice.
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene, 45.f, true);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(12.0, 3.0));

    const Runtime::SkyResolution summer = ResolveSky(scene);
    scene.GetMut<TimeOfDay>(entity)->dayOfYear = 9.0;
    const Runtime::SkyResolution winter = ResolveSky(scene);

    REQUIRE(summer.declinationDegrees > 20.f);
    REQUIRE(winter.declinationDegrees < -20.f);
    CHECK(summer.daylightHours > winter.daylightHours + 6.f);

    const float summerBlue = summer.light.color.b / summer.light.color.r;
    const float winterBlue = winter.light.color.b / winter.light.color.r;
    CHECK(winterBlue < summerBlue);
    CHECK(winter.light.color.r < summer.light.color.r);
}

TEST_CASE("Polar night is a steady state with no lighting body and nothing to shadow")
{
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene, 80.f);
    // A long year, so the declination stays deep across the days sampled and this
    // is a claim about a season rather than about one midnight.
    (void)scene.Add<TimeOfDay>(entity, ClockAt(12.0, 2737.0, 3650.f));

    for (int32_t offset = 0; offset < 5; ++offset)
    {
        for (int32_t hour = 0; hour < 24; ++hour)
        {
            CAPTURE(offset);
            CAPTURE(hour);
            TimeOfDay *clock = scene.GetMut<TimeOfDay>(entity);
            clock->dayOfYear = 2737.0 + static_cast<double>(offset);
            clock->hour = static_cast<double>(hour);

            const Runtime::SkyResolution sky = ResolveSky(scene);
            REQUIRE(sky.sun.directionToSun.y < 0.f);
            // No moon in this scene, so nothing lights — and the shadow flag goes
            // with it, which is what makes three weeks of this cost nothing.
            CHECK(sky.light.body == LightingBody::None);
            CHECK(sky.light.intensity == 0.f);
            CHECK_FALSE(sky.light.castsShadows);
            CHECK(sky.daylightHours == doctest::Approx(0.f));
        }
    }
}

TEST_CASE("A clocked sun over a level with no atmosphere still lights it")
{
    ECS::Scene scene;
    const ECS::Entity entity = AddSun(scene, glm::vec3(0.f, -1.f, 0.f), false);
    (void)scene.Add<Sun>(entity, Sun{.latitudeDegrees = 0.f});
    (void)scene.Add<TimeOfDay>(entity, ClockAt(12.0));

    const Runtime::SkyResolution sky = ResolveSky(scene);
    // No Skybox, so no sky is drawn — but the light is real and the clock aims it.
    CHECK(sky.status == SkyStatus::NoSkybox);
    CHECK(sky.light.body == LightingBody::Sun);
    CHECK(sky.light.intensity > 0.f);
    CHECK(sky.sun.directionToSun.y == doctest::Approx(1.f).epsilon(1e-4));
}

TEST_CASE("The moon's image up is a usable frame wherever the moon is")
{
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene, 67.f);
    (void)scene.Add<Moon>(entity);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(0.0));

    for (int32_t step = 0; step < 240; ++step)
    {
        CAPTURE(step);
        TimeOfDay *clock = scene.GetMut<TimeOfDay>(entity);
        clock->day = step / 24;
        clock->hour = static_cast<double>(step % 24);

        const Runtime::SkyResolution sky = ResolveSky(scene);
        REQUIRE(glm::length(sky.moon.imageUp) == doctest::Approx(1.f).epsilon(1e-4));
        REQUIRE(std::abs(glm::dot(sky.moon.imageUp, sky.moon.directionToMoon)) < 1e-4f);
    }
}

TEST_CASE("A Moon without a Sun is ignored, and a moon below the horizon lights nothing")
{
    ECS::Scene scene;
    const ECS::Entity entity = AddSun(scene, glm::vec3(0.f, -1.f, 0.f), false);
    (void)scene.Add<Skybox>(entity);
    (void)scene.Add<Moon>(entity);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(0.0));

    // No Sun, so no ecliptic for the orbit to be defined against.
    const Runtime::SkyResolution orphaned = ResolveSky(scene);
    CHECK(orphaned.moon.intensity == 0.f);
    CHECK(orphaned.moon.diskIntensity == 0.f);

    (void)scene.Add<Sun>(entity, Sun{.latitudeDegrees = 45.f});
    // Noon with a new moon: the moon is at the sun's position, so it is up — and
    // the night gate is shut anyway.
    scene.GetMut<TimeOfDay>(entity)->hour = 12.0;
    scene.GetMut<Moon>(entity)->phaseAtEpoch = 0.5f;
    const Runtime::SkyResolution daytime = ResolveSky(scene);
    // A full moon at noon is below the horizon, opposite the sun.
    REQUIRE(daytime.moon.directionToMoon.y < 0.f);
    CHECK(daytime.light.body == LightingBody::Sun);
}

TEST_CASE("Two suns leave every aim authored, clock or no clock")
{
    ECS::Scene scene;
    const glm::vec3 authored = glm::normalize(glm::vec3(1.f, -1.f, 0.f));
    const ECS::Entity first = AddSun(scene, authored, false);
    (void)scene.Add<Skybox>(first);
    (void)scene.Add<Sun>(first, Sun{});
    (void)scene.Add<TimeOfDay>(first, ClockAt(6.0));
    (void)AddSun(scene, glm::vec3(0.f, -1.f, 0.f), false);

    const Runtime::SkyResolution sky = ResolveSky(scene);
    CHECK(sky.status == SkyStatus::MultipleDirectionalLights);
    // Nothing lights: with two suns the shadowed one and the drawn one could
    // disagree, and a clock driving one would make that disagreement move.
    CHECK(sky.light.body == LightingBody::None);
    CHECK(sky.light.entity == ECS::NullEntity);
}

TEST_CASE("A jump shows up in the resolution, and a plain advance does not")
{
    ECS::Scene scene;
    const ECS::Entity entity = AddClockedSun(scene);
    (void)scene.Add<TimeOfDay>(entity, ClockAt(12.0));

    const std::uint32_t before = ResolveSky(scene).jumpSerial;

    Assisi::Runtime::AdvanceTimeOfDay(*scene.GetMut<TimeOfDay>(entity), 1.f);
    CHECK(ResolveSky(scene).jumpSerial == before);

    // What the shadow cadence watches: a cut, as against the sun merely moving
    // fast. Both move the sun; only one invalidates history.
    CHECK(Assisi::Runtime::JumpForwardTo(*scene.GetMut<TimeOfDay>(entity), 6.0));
    CHECK(ResolveSky(scene).jumpSerial == before + 1);
}
