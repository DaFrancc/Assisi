/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyComponents.hpp
/// @brief The Skybox component — a sky, authored on the light that lights it.
///
/// Placement: Skybox goes on an entity that also carries a DirectionalLight, and
/// that light is the sun the sky scatters. A DirectionalLight on its own is just
/// a light; the sky is what the component opts into. See Runtime::ResolveSky for
/// the rules, including what a scene with several directional lights does.
///
/// Two more components may sit on that same entity, and neither is required: a
/// Sun makes the light's aim come from the scene's clock instead of from its
/// `direction`, and a Moon beside it puts a second body in this sky. Both are in
/// TimeOfDay.hpp.
///
/// Everything the sky's look needs is here and saved with the level, so a world
/// is authored in the editor rather than in engine code.
///
/// The Fog component is here too: it takes its colour from this sky, but it is
/// not placed by it and works in a level with none. So is Clouds, which is part
/// of the sky and goes on its entity.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Clouds.hpp>
#include <Assisi/Render/Fog.hpp>
#include <Assisi/Render/Sky.hpp>

#include <array>
#include <cstddef>

namespace Assisi::Runtime
{

/// @brief A named sky, for authors who want a place rather than a set of
/// coefficients.
///
/// These are conditions, not planets: what separates them is how clean the air
/// is, what is suspended in it, and what the ground under it reflects. A preset
/// writes every knob below, so picking one and reading the numbers it produced
/// is the fastest way to learn what any of them do.
AENUM()
enum class SkyPreset : std::uint8_t
{
    /// A temperate clear day. The baseline the defaults describe.
    Clear,
    /// Cold, dry, very clean air over snow. Deep blue overhead, almost no haze,
    /// and a bright ground that throws a great deal back up.
    Arctic,
    /// Hot and dusty. Warm-tinted haze thick enough to pale the sky toward the
    /// horizon, over dry ground.
    Savanna,
    /// Humid and hazy. Moisture scatters greyly and hardest toward the sun, so
    /// the horizon washes out early and the sky reads soft.
    Tropical,
    /// Thin air at altitude. Little of it to scatter in, so the zenith goes
    /// nearly navy and the sun stays hard and white far into the afternoon.
    Alpine,
    /// Thick, still, muggy air. The sun survives as a pale disk with no edge to
    /// it and the whole sky flattens toward grey.
    ///
    /// Deliberately NOT called overcast, and it is worth saying why. A cloudy sky
    /// is bright because light bounces many times inside the cloud; single
    /// scattering has no way to reach that, and haze thick enough to bury the sun
    /// here takes the sky down with it. This is the thickest air the model is
    /// honest about.
    Hazy,
    /// No atmosphere. A black sky, a hard white sun, and lit ground — the
    /// airless limit of the same model rather than a mode of its own.
    Airless,
    /// The knobs below, as authored. Every preset greys them out, because a
    /// preset IS the answer — an author who wants to depart from one starts here.
    Custom,
};

/// @brief An analytic atmosphere, scattering the light on the same entity.
///
/// The sun's colour and intensity are deliberately NOT here: they are the
/// DirectionalLight's, because they are one physical quantity. The same value
/// lights every surface in the world, tints the scattering, and colours the
/// disk — so a blue sun is a blue directional light and everything follows from
/// it. @ref sunDiskColor tints the disk alone, for when that coherence is not
/// what is wanted.
///
/// The scattering coefficients are the air's composition, not Earth's constants.
/// Pushing them green gives a green noon and a magenta sunset with nothing
/// further authored, because the same coefficient governs both. Setting
/// @ref airThickness to zero is a world with no atmosphere at all: a black
/// sky, a hard unattenuated sun, and a lit ground.
ACOMP()
struct Skybox
{
    /// Which sky this is. Every value but Custom answers the whole component, so
    /// the knobs below grey out — a greyed field is one nothing reads.
    ///
    /// A preset is a live answer rather than a stamp: a level saved as Arctic
    /// follows the Arctic numbers if they are ever improved, where one saved as
    /// Custom keeps exactly what was typed.
    AFIELD(radioBroadcast) SkyPreset preset = SkyPreset::Clear;

    /// @name The air
    /// @{
    /// Scattering by the air's own molecules, per channel. Earth's falls as the
    /// inverse fourth power of wavelength, which is why its sky is blue and its
    /// sunsets are red — both from this one triple.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 airScattering = Assisi::Render::kEarthAirScattering;

    /// How much air there is, scaling the coefficients above. Zero is airless.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float airThickness = Assisi::Render::kDefaultAirThickness;

    /// Dust and droplets, per channel. Grey on Earth; colouring it is what a
    /// dust-dominated sky needs.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 hazeScattering = Assisi::Render::kEarthHazeScattering;

    /// How sharply haze throws light. Positive scatters forward (the halo around
    /// a low sun), negative scatters back toward it, zero is uniform.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) float hazeForwardness = Assisi::Render::kDefaultHazeForwardness;

    /// How much light knocked out of the direct line of sight arrives anyway,
    /// having bounced again. Without it the horizon turns green, because single
    /// scattering treats that light as lost exactly where there is most of it.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float skyBounce = Assisi::Render::kDefaultSkyBounce;
    /// @}

    /// @name Colour
    /// @{
    /// Albedo of the ground half of the sphere, lit by the same sun as the sky.
    /// Not a real surface — the sky covers every direction, and this is what the
    /// ones pointing down get.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 groundColor{0.11f, 0.10f, 0.09f};

    /// What is left when the sun has gone, added rather than blended so no
    /// threshold has to decide when night starts.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 nightColor = Assisi::Render::kDefaultNightColor;

    /// Exposure of the sky against the rest of the scene. Far from 1 because the
    /// model yields the fraction of the beam a column of air scatters toward the
    /// eye, which for a clear zenith is a few percent.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float exposure = Assisi::Render::kDefaultSkyExposure;
    /// @}

    /// @name The disk
    /// @{
    /// Angular radius in degrees. Earth's sun is about 0.27, but a disk that
    /// small lands inside one pixel and aliases into a crawling dot.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunSizeDegrees = Assisi::Render::kDefaultSunSizeDegrees;

    /// Width of the edge fade as a fraction of the radius. Never zero: the disk
    /// is the brightest thing in the frame by orders of magnitude, and a hard
    /// edge on it flickers as the camera turns.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunEdgeSoftness = Assisi::Render::kDefaultSunEdgeSoftness;

    /// How much dimmer the rim is than the centre. A star is a ball of gas, not
    /// a sticker — zero is a flat disk, and a flat disk is what reads as fake.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunLimbDarkening = Assisi::Render::kDefaultSunLimbDarkening;

    /// Brightness of the disk against the sky around it. Zero removes the disk
    /// and leaves the scattering that surrounds it untouched.
    AFIELD(min = 0, radioListen = {source = preset, value = Custom, behavior = grey}) float sunDiskIntensity = Assisi::Render::kDefaultSunDiskIntensity;

    /// Tint on the disk ALONE, over the light's own colour. White leaves the
    /// disk agreeing with what lights the world; pushing it is the escape hatch
    /// for a yellow sun over a neutrally-lit scene.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) glm::vec3 sunDiskColor{1.0f, 0.95f, 0.82f};
    /// @}

    /// @name The playable floor
    ///
    /// **Deliberately NOT greyed by the preset, and that is the whole point.** A
    /// preset answers for the air — what this planet's atmosphere is made of and
    /// how much of it there is. How dark a game is willing to get is not a fact
    /// about the air, and an Arctic sky prescribing it would be a preset
    /// answering a question it was never asked.
    /// @{

    /// The least the sky may light the world, whatever the hour.
    ///
    /// A moonless night is very nearly black, which is correct and is often not
    /// playable. This is the floor under the sky's own answer: it only ever
    /// raises, so by day it is inert — the daytime term is orders above any
    /// sensible floor — and at night it is what keeps a scene readable.
    ///
    /// It floors the AMBIENT, not the sky's radiance, so the night sky stays as
    /// dark as it is while the ground under it becomes visible. Flooring the
    /// radiance instead would light the world by making the sky glow.
    ///
    /// **It has no direction and casts no shadows.** The indirect term is a
    /// hemisphere, so it still shades an upward-facing surface differently from a
    /// downward-facing one — but nothing is up there to cast from, which is also
    /// true of the real thing. A night with shadows in it wants a moon, not a
    /// bigger floor.
    ///
    /// Zero is off, and is the default: a level that says nothing gets the
    /// physical answer.
    AFIELD(min = 0, max = 10.0) float minimumAmbient = 0.f;

    /// The hue that floor takes. Read only when @ref minimumAmbient is above
    /// zero; kept authored either way so switching the floor on gives something
    /// usable rather than grey.
    AFIELD() glm::vec3 minimumAmbientColor{0.35f, 0.45f, 0.7f};
    /// @}
};

/// @brief The level's fog: a haze that thickens with distance, and a mist that
/// lies low in the valleys.
///
/// One per level, on any entity; with more than one, the first found is used
/// and the log says so. Without one a level has no fog and pays nothing for it.
///
/// The colour comes from the sky in the direction each pixel looks, so the fog
/// is warm toward a sunset and dark at night with nothing more authored. Only a
/// level with no Skybox reads @ref color.
///
/// The haze's @ref hazeVisibility is also the draw distance: nothing past it is
/// drawn, and the camera's farZ is shortened to it. Whatever limits the draw
/// distance, everything fades into the fog's colour before it, so the edge of
/// the world never shows as a wall.
ACOMP()
struct Fog
{
    /// The fog's colour where the level has no sky to take one from.
    AFIELD() glm::vec3 color = Assisi::Render::kDefaultFogColor;

    /// @name The haze
    /// @{
    /// How far you can see through the haze where it is thickest, in metres.
    /// Nothing past it is drawn. Zero is no haze.
    AFIELD(min = 0) float hazeVisibility = 0.f;
    /// Below this world height the haze is at its thickest.
    AFIELD() float hazeHeight = 0.f;
    /// How many metres above @ref hazeHeight the haze takes to thin to about a
    /// third. Zero keeps it the same at every height.
    AFIELD(min = 0) float hazeFalloff = 0.f;
    /// @}

    /// @name The mist
    /// @{
    /// How far you can see through the mist where it is thickest, in metres.
    /// Zero is no mist. Unlike the haze, it never limits the draw distance.
    AFIELD(min = 0) float mistVisibility = 0.f;
    /// Below this world height the mist is at its thickest: the valley floor
    /// it fills.
    AFIELD() float mistHeight = 0.f;
    /// How many metres above @ref mistHeight the mist takes to thin to about a
    /// third. Small values give it a visible top.
    AFIELD(min = 0) float mistFalloff = 0.f;
    /// @}
};

/// @brief A named kind of cloudy sky.
AENUM()
enum class CloudPreset : std::uint8_t
{
    /// Fair weather: small cumulus, well apart, with plenty of blue between.
    Fair,
    /// Cumulus over about half the sky.
    Scattered,
    /// More cloud than sky, with gaps the sun breaks through.
    Broken,
    /// A low grey layer closing the sky. No sun shadows under it.
    Overcast,
    /// The fields below, as authored.
    Custom,
};

/// @brief Clouds in the sky, lit by the same sun and moon, casting shadows on
/// the ground and greying the sky's light as they close it.
///
/// Goes on the sun's entity, beside its Skybox: the clouds are part of that
/// sky. Without a Skybox there they are not drawn, and the log says so. Without
/// this component a level has no clouds and pays nothing for them.
///
/// The wind carries them as time passes, in the editor as in the game, whether
/// or not the scene's clock runs. Each machine moves its own: clouds are
/// scenery, and nothing in play depends on where they are.
ACOMP()
struct Clouds
{
    /// Which kind of sky this is. Every value but Custom answers the five
    /// fields below, which grey out. The change rate, the wind and the seed are
    /// never greyed: they work with every preset.
    AFIELD(radioBroadcast) CloudPreset preset = CloudPreset::Scattered;

    /// How much of the sky is cloud, from 0 (clear) to 1 (overcast).
    AFIELD(min = 0, max = 1, radioListen = {source = preset, value = Custom, behavior = grey}) float coverage = Assisi::Render::kDefaultCloudCoverage;
    /// The height of the clouds' flat bases, in metres above the world's zero.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) float height = Assisi::Render::kDefaultCloudBaseHeight;
    /// How tall the clouds can grow above their bases, in metres.
    AFIELD(min = 100, radioListen = {source = preset, value = Custom, behavior = grey}) float thickness = Assisi::Render::kDefaultCloudThickness;
    /// How thick a cloud is inside. Higher hides the sun behind smaller clouds
    /// and darkens their undersides.
    AFIELD(min = 0, max = 1, radioListen = {source = preset, value = Custom, behavior = grey}) float density = Assisi::Render::kDefaultCloudDensity;
    /// How heaped the clouds are, from 0 (flat grey sheets) to 1 (towering
    /// cumulus). The sky mixes kinds around it, so towers rise among lower
    /// clouds and sheets lie among heaps.
    AFIELD(min = 0, max = 1, radioListen = {source = preset, value = Custom, behavior = grey}) float puffiness = Assisi::Render::kDefaultCloudPuffiness;

    /// How fast the clouds change shape as they drift: 0 never, 1 about as fast
    /// as real cumulus, which form and fade over a quarter of an hour.
    AFIELD(min = 0, max = 10) float changeRate = Assisi::Render::kDefaultCloudChangeRate;

    /// How fast the wind carries the clouds, in metres per second.
    AFIELD(min = 0, max = 100) float windSpeed = Assisi::Render::kDefaultCloudWindSpeed;
    /// The compass bearing they move toward: 0 north, 90 east.
    AFIELD() float windBearingDegrees = Assisi::Render::kDefaultCloudWindBearingDegrees;
    /// Which sky of this kind: the same settings with another seed give other
    /// clouds.
    AFIELD() uint32_t seed = 0;
};

/// @brief The component as the render side wants it.
[[nodiscard]] inline Assisi::Render::FogSettings ToFogSettings(const Fog &fog)
{
    return Assisi::Render::FogSettings{.color = fog.color,
                                       .hazeVisibility = fog.hazeVisibility,
                                       .hazeHeight = fog.hazeHeight,
                                       .hazeFalloff = fog.hazeFalloff,
                                       .mistVisibility = fog.mistVisibility,
                                       .mistHeight = fog.mistHeight,
                                       .mistFalloff = fog.mistFalloff};
}

/// @brief Every preset, already built, in the order @ref SkyPreset declares them.
///
/// A table rather than a function that assembles one per call: these are seven
/// fixed answers, so they are constructed once at compile time and handed out.
///
/// Each is written as what it CHANGES. The fields left out are not missing — an
/// aggregate fills them from SkySettings' own defaults — so a preset reads as
/// the handful of knobs that make it what it is, and stays a complete answer:
/// an author switching from Arctic to Savanna gets the savanna's ground as well
/// as its dust, with nothing surviving from the sky before it.
inline constexpr std::array<Assisi::Render::SkySettings, static_cast<std::size_t>(SkyPreset::Custom) + 1>
kSkyPresets{{
    // Clear: the defaults, unchanged. It is the sky every other entry is a
    // departure from.
    Assisi::Render::SkySettings{},

    // Arctic. Cold air holds almost no moisture, so there is very little to
    // scatter greyly and the blue survives all the way to the horizon. Snow
    // is the brightest ground there is, and most of what the sky sends down
    // comes straight back — which is why the light under it is so flat: the
    // fill from below rivals the sun from above, and nothing has a dark side.
    Assisi::Render::SkySettings{.airThickness = 0.22f,
                                .hazeScattering = glm::vec3(0.0010f),
                                .skyBounce = 0.50f,
                                .groundColor = glm::vec3(0.90f, 0.93f, 0.97f)},

    // Savanna. Suspended dust, which scatters red where air scatters blue,
    // so the haze itself is warm and the horizon goes straw before the sun
    // is anywhere near it. Enough of it that a shadow here is lit by warm
    // light rather than by a blue sky — which is what makes the place read
    // as hot rather than merely bright.
    Assisi::Render::SkySettings{.airThickness = 0.13f,
                                .hazeScattering = glm::vec3(0.050f, 0.032f, 0.012f),
                                .hazeForwardness = 0.72f,
                                .groundColor = glm::vec3(0.42f, 0.32f, 0.16f)},

    // Tropical. Water droplets are large enough to scatter every wavelength
    // alike and hard forward, so the sky pales toward the sun and the horizon
    // loses its colour long before the ground does. Thick enough to take real
    // bite out of the sun: the light here is mostly a bright white sky rather
    // than a beam, and the ground throws green back into everything above it.
    Assisi::Render::SkySettings{.airThickness = 0.10f,
                                .hazeScattering = glm::vec3(0.055f),
                                .hazeForwardness = 0.88f,
                                .skyBounce = 0.30f,
                                .groundColor = glm::vec3(0.09f, 0.16f, 0.07f)},

    // Alpine. Less air overhead means less of it to scatter in: the zenith
    // darkens toward navy, and the sun keeps its edge and its colour much
    // lower in the sky than it would at sea level. The hardest light of the
    // seven — a nearly white beam against almost no sky to fill the shadows,
    // over pale rock that is the only thing softening them.
    Assisi::Render::SkySettings{.airThickness = 0.035f,
                                .hazeScattering = glm::vec3(0.0004f),
                                .skyBounce = 0.10f,
                                .groundColor = glm::vec3(0.50f, 0.51f, 0.54f),
                                .sunEdgeSoftness = 0.05f},

    // Hazy. Enough suspended water to take most of the direct sun out and
    // spread it, so the disk survives as something you can look at and the
    // sky loses its blue. The wide, soft edge is what a sun seen through haze
    // has; a hard one would read as a hole punched in fog.
    //
    // The haze is at the ceiling of what single scattering is honest about,
    // which is what makes this the flattest sky here rather than the darkest:
    // sky and ground arrive at nearly the same value, so a surface's own
    // shape stops telling you which way it faces.
    Assisi::Render::SkySettings{.airThickness = 0.16f,
                                .hazeScattering = glm::vec3(0.060f),
                                .hazeForwardness = 0.55f,
                                .skyBounce = 0.35f,
                                .groundColor = glm::vec3(0.20f, 0.20f, 0.19f),
                                .exposure = 6.5f,
                                .sunEdgeSoftness = 0.40f,
                                .sunDiskIntensity = 8.0f},

    // Airless. The degenerate case, and it needs no special handling
    // anywhere: with nothing to scatter in, the scattering term is zero and
    // the beam arrives whole.
    Assisi::Render::SkySettings{.airThickness = 0.0f,
                                .hazeScattering = glm::vec3(0.0f),
                                .groundColor = glm::vec3(0.14f, 0.13f, 0.12f),
                                .nightColor = glm::vec3(0.0f),
                                .sunEdgeSoftness = Assisi::Render::kMinSunEdgeSoftness,
                                .sunDiskColor = glm::vec3(1.0f)},

    // Custom: the defaults again, which is what makes departing from a preset
    // start where the knobs already are rather than somewhere else.
    Assisi::Render::SkySettings{},
}};

// Custom is last, and this is what says so. Adding a preset anywhere shifts
// Custom's value, and the count stops matching until the table gains its row —
// which is the check the switch this replaced got from -Wswitch for free.
static_assert(kSkyPresets.size() == static_cast<std::size_t>(SkyPreset::Custom) + 1,
              "Every SkyPreset needs a row in kSkyPresets, and Custom has to stay last.");

/// @brief The settings a named sky stands for.
[[nodiscard]] inline const Assisi::Render::SkySettings &PresetSettings(SkyPreset preset)
{
    return kSkyPresets[static_cast<std::size_t>(preset)];
}

/// @brief The component's look as the render side wants it.
[[nodiscard]] inline Assisi::Render::SkySettings ToSkySettings(const Skybox &skybox)
{
    // A preset answers outright. The stored knobs are not consulted and not
    // merged into — which is what lets the inspector grey them honestly.
    if (skybox.preset != SkyPreset::Custom)
    {
        return PresetSettings(skybox.preset);
    }

    return Assisi::Render::SkySettings{.airScattering = skybox.airScattering,
                                       .airThickness = skybox.airThickness,
                                       .hazeScattering = skybox.hazeScattering,
                                       .hazeForwardness = skybox.hazeForwardness,
                                       .skyBounce = skybox.skyBounce,
                                       .groundColor = skybox.groundColor,
                                       .nightColor = skybox.nightColor,
                                       .exposure = skybox.exposure,
                                       .sunSizeDegrees = skybox.sunSizeDegrees,
                                       .sunEdgeSoftness = skybox.sunEdgeSoftness,
                                       .sunLimbDarkening = skybox.sunLimbDarkening,
                                       .sunDiskIntensity = skybox.sunDiskIntensity,
                                       .sunDiskColor = skybox.sunDiskColor};
}

/// @brief Every cloud preset, in the order @ref CloudPreset declares them. The
/// change rate, the wind and the seed are left at their defaults: the
/// component's own always replace them.
inline constexpr std::array<Assisi::Render::CloudSettings, static_cast<std::size_t>(CloudPreset::Custom) + 1>
kCloudPresets{{
    // Fair: a third of the sky, in heaped clouds shorter than they are wide.
    Assisi::Render::CloudSettings{.coverage = 0.3f, .baseHeight = 1500.0f, .thickness = 1500.0f, .puffiness = 1.0f},

    // Scattered: the defaults.
    Assisi::Render::CloudSettings{},

    // Broken: lower, taller and denser, with flatter cloud among the heaps, so
    // the gaps are what stand out.
    Assisi::Render::CloudSettings{
        .coverage = 0.7f, .baseHeight = 1200.0f, .thickness = 2500.0f, .density = 0.11f, .puffiness = 0.6f},

    // Overcast: a low, closed sheet of stratus, dense enough that the sun
    // shows through nowhere.
    Assisi::Render::CloudSettings{
        .coverage = 0.97f, .baseHeight = 800.0f, .thickness = 1500.0f, .density = 0.08f, .puffiness = 0.15f},

    // Custom: the defaults again, so departing from a preset starts where the
    // fields already are.
    Assisi::Render::CloudSettings{},
}};

static_assert(kCloudPresets.size() == static_cast<std::size_t>(CloudPreset::Custom) + 1,
              "Every CloudPreset needs a row in kCloudPresets, and Custom has to stay last.");

/// @brief The component's clouds as the render side wants them.
[[nodiscard]] inline Assisi::Render::CloudSettings ToCloudSettings(const Clouds &clouds)
{
    Assisi::Render::CloudSettings settings = kCloudPresets[static_cast<std::size_t>(clouds.preset)];
    if (clouds.preset == CloudPreset::Custom)
    {
        settings.coverage = clouds.coverage;
        settings.baseHeight = clouds.height;
        settings.thickness = clouds.thickness;
        settings.density = clouds.density;
        settings.puffiness = clouds.puffiness;
    }
    settings.changeRate = clouds.changeRate;
    settings.windSpeed = clouds.windSpeed;
    settings.windBearingDegrees = clouds.windBearingDegrees;
    settings.seed = clouds.seed;
    return Assisi::Render::Sanitized(settings);
}

} // namespace Assisi::Runtime
