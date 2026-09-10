/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SkyResolve.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/Celestial.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/SkyComponents.hpp>
#include <Assisi/Runtime/TimeOfDay.hpp>

#include <cmath>

namespace Assisi::Runtime
{

namespace
{
/// The scene's clock, or the defaults if it has none.
///
/// A Sun without a TimeOfDay evaluates at noon on an equinox rather than
/// refusing: the aim then comes from the place and the planet, which is a
/// complete answer, and it is what an author gets while assembling the entity one
/// component at a time.
TimeOfDay ClockOf(ECS::Scene &scene, bool &sawAny)
{
    std::uint32_t count = 0;
    TimeOfDay found;
    for (auto [entity, clock] : scene.Query<TimeOfDay>())
    {
        (void)entity;
        ++count;
        if (count == 1)
        {
            found = clock;
        }
    }
    sawAny = count > 0;
    if (count > 1)
    {
        Core::Log::Warn("ResolveSky: {} TimeOfDay components in the scene; the first is the clock.", count);
    }
    return found;
}
} // namespace

SkyResolution ResolveSky(ECS::Scene &scene)
{
    ECS::Entity sunEntity = ECS::NullEntity;
    const DirectionalLight *sunLight = nullptr;
    std::uint32_t directionalCount = 0;

    for (auto [entity, light] : scene.Query<DirectionalLight>())
    {
        ++directionalCount;
        if (directionalCount > 1)
        {
            // Kept walking rather than broken out of, because the count is the
            // answer and stopping at two would only save a handful of entities.
            continue;
        }
        sunEntity = entity;
        sunLight = &light;
    }

    if (directionalCount == 0)
    {
        SkyResolution none;
        none.status = SkyStatus::NoDirectionalLight;
        return none;
    }
    if (directionalCount > 1)
    {
        // Every aim stays authored. With two suns the shadowed one and the drawn
        // one could disagree, and a clock driving one of them would make that
        // disagreement move.
        SkyResolution ambiguous;
        ambiguous.status = SkyStatus::MultipleDirectionalLights;
        return ambiguous;
    }

    const Skybox *skybox = scene.Get<Skybox>(sunEntity);
    const Sun *sunPlace = scene.Get<Sun>(sunEntity);
    const Moon *moonBody = sunPlace != nullptr ? scene.Get<Moon>(sunEntity) : nullptr;
    if (sunPlace == nullptr && scene.Get<Moon>(sunEntity) != nullptr)
    {
        Core::Log::Warn("ResolveSky: a Moon without a Sun has no ecliptic to orbit; it is ignored.");
    }

    bool haveClock = false;
    const TimeOfDay clock = Sanitized(ClockOf(scene, haveClock));
    if (sunPlace != nullptr && !haveClock)
    {
        Core::Log::Warn("ResolveSky: a Sun with no TimeOfDay in the scene stands at noon on an equinox.");
    }

    SkyResolution resolution;
    resolution.settings = skybox != nullptr ? ToSkySettings(*skybox) : Render::SkySettings{};
    if (skybox != nullptr)
    {
        // Premultiplied here so everything downstream sees one colour and no
        // second knob. Sanitized against the same range every other sky channel
        // is, because it reaches a shader by the same route they do.
        resolution.minimumAmbient = Render::SanitizedSkyChannels(
            skybox->minimumAmbientColor * std::max(skybox->minimumAmbient, 0.f), glm::vec3(0.f));
    }
    resolution.hour = clock.hour;
    resolution.day = clock.day;
    resolution.dayOfYear = clock.dayOfYear;
    resolution.jumpSerial = clock.jumpSerial;

    // Where the sun is. From the clock when the entity says where on the planet it
    // is standing, and from the authored aim otherwise — a light placed by hand
    // for one shot is not on anybody's clock.
    glm::vec3 toSun = Render::SafeSkyDirection(-sunLight->direction);
    if (sunPlace != nullptr)
    {
        const Render::SkyClock angles = ClockAngles(clock);
        const Render::Observer observer = ObserverOf(*sunPlace);
        toSun = Render::SunDirection(angles, observer);

        const float sinDeclination = Render::SinDeclination(observer.axialTiltRadians, angles.solarLongitude);
        resolution.declinationDegrees = glm::degrees(std::asin(std::clamp(sinDeclination, -1.f, 1.f)));
        resolution.daylightHours = Render::DaylightHours(observer.latitudeRadians, sinDeclination);
        resolution.sunAngularVelocity = SunAngularVelocity(clock);

        if (moonBody != nullptr)
        {
            const Moon moon = Sanitized(*moonBody);
            const glm::vec3 toMoon = Render::MoonDirection(angles, observer, OrbitOf(clock, moon));
            resolution.moon = Render::SkyMoon{
                .directionToMoon = toMoon,
                .imageUp = Render::MoonImageUp(toMoon, Render::EclipticPole(angles, observer)),
                .color = glm::vec3(moon.color),
                .intensity = moon.intensity,
                .sizeDegrees = moon.sizeDegrees,
                .diskColor = glm::vec3(moon.diskColor),
                .diskIntensity = moon.diskIntensity,
                .atmosphericTint = moon.atmosphericTint};
            resolution.moonLitFraction = Render::MoonLitFraction(toSun, toMoon);
            resolution.moonAngularVelocity = MoonAngularVelocity(clock, moon);
        }
    }

    // What the SKY scatters is the raw authored radiance, unramped and at every
    // hour including under the horizon. The ramps below are about what lights the
    // world; applying them here would cut the sunset off at the moment the sun
    // touched the horizon, which is when a sunset is only starting.
    //
    // While tintedBySky is on the authored colour is not read anywhere — the
    // inspector greys it out, and a greyed field that still tinted the sky would
    // make that grey a lie. The sun above the air is white, and the atmosphere
    // does the rest.
    resolution.sun = Render::SkySun{.directionToSun = toSun,
                                    .color = sunLight->tintedBySky ? glm::vec3(1.f) : AuthoredSunColor(*sunLight),
                                    .intensity = sunLight->intensity};

    const Render::SkySettings *atmosphere =
        (skybox != nullptr && sunLight->tintedBySky) ? &resolution.settings : nullptr;

    // The handoff. Both factors are exactly zero at the geometric horizon, so the
    // body holding the slot changes where NEITHER is lighting anything — which is
    // the only place it can change without a visible step, since at any other
    // threshold the two are equal and non-zero rather than both dark.
    const float sunRamp = Render::HorizonRamp(toSun.y);
    const float sunIntensity = sunLight->intensity * sunRamp;

    float moonIntensity = 0.f;
    if (resolution.moon.intensity > 0.f)
    {
        // The phase is in the light as well as on the disk. Without it a new moon
        // would light the ground as brightly as a full one — mostly hidden by the
        // geometry, since a new moon sets with the sun, but a thin crescent
        // lingering after sunset would give the whole night away.
        moonIntensity = resolution.moon.intensity * resolution.moonLitFraction *
                        Render::HorizonRamp(resolution.moon.directionToMoon.y) * Render::NightGate(toSun.y);
    }

    // Not a tiebreak, and there is no tie to break: HorizonRamp is nonzero only
    // above the horizon and NightGate only below it, so at most one of these two
    // is ever nonzero. A moon high in the daytime sky lights nothing and casts
    // nothing — which is what the real one does, at a millionth of the sun — while
    // still being drawn at its own size and phase and still scattering into the
    // sky beside it.
    //
    // The entity and the aim are set BEFORE the branches, and unconditionally.
    // The row belongs to this light whether or not anything is currently lighting
    // it: Gather claims a row by matching this entity, so leaving it null through
    // a night with no moon would leave the row unclaimed, and Gather would fall
    // back to the authored aim — a sun at full intensity, pointing straight down,
    // in the middle of the night. "Nothing is lighting" has to be something the
    // resolver SAYS, not something it declines to say.
    resolution.light.entity = sunEntity;
    resolution.light.direction = -toSun;

    if (sunIntensity > 0.f)
    {
        resolution.light.color = LightingSystem::SunlightColor(AuthoredSunColor(*sunLight), toSun, atmosphere);
        resolution.light.intensity = sunIntensity;
        resolution.light.castsShadows = sunLight->castsShadows;
        resolution.light.body = LightingBody::Sun;
    }
    else if (moonIntensity > 0.f)
    {
        const glm::vec3 toMoon = resolution.moon.directionToMoon;
        // The moon's light on the world is its own colour, not the photograph's:
        // the texture is albedo on a disk, and what lights a landscape is the
        // integrated body.
        resolution.light.direction = -toMoon;
        // The moon's own colour THROUGH the air, not the air alone.
        //
        // Not LightingSystem::SunlightColor, which deliberately discards the
        // colour it is handed: for the sun that is right, because tintedBySky
        // means the sky IS the colour and the inspector greys the authored one
        // out to say so. Nothing greys Moon::color, so throwing it away would
        // make that field a lie — and would leave the moon reddening from white,
        // which is why a low moon looked like a small sunset instead of a warm
        // moon.
        //
        // The reddening is real — a low moon crosses the same long path of air a
        // low sun does — but only as much of it as the moon's own tint asks for,
        // and the same amount the disk takes, so the world and the thing lighting
        // it agree. See SkyMoon::atmosphericTint for why that is not all of it.
        glm::vec3 throughAir(1.f);
        if (atmosphere != nullptr)
        {
            throughAir = Render::TintedTransmittance(Render::SunlightTransmittance(toMoon, *atmosphere),
                                                     resolution.moon.atmosphericTint);
        }
        resolution.light.color = resolution.moon.color * throughAir;
        resolution.light.intensity = moonIntensity;
        resolution.light.castsShadows = sunLight->castsShadows;
        resolution.light.body = LightingBody::Moon;
    }

    resolution.status = skybox != nullptr ? SkyStatus::Ready : SkyStatus::NoSkybox;
    return resolution;
}

} // namespace Assisi::Runtime
