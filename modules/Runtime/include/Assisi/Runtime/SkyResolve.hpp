/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyResolve.hpp
/// @brief Which sky a scene has, if any, and where its bodies are right now.
///
/// The components this reads all live on one entity: a DirectionalLight, a
/// Skybox for the air, a Sun for the place on the planet, and a Moon for a
/// second body. Only the light is required. The clock itself — TimeOfDay — is
/// conventionally on the same entity but is looked up scene-wide, because there
/// is one time of day and it is not the sun's property.

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Sky.hpp>

#include <cstdint>

namespace Assisi::Runtime
{

/// @brief Why a scene does or does not draw a sky.
enum class SkyStatus : std::uint8_t
{
    /// A sky, and the sun that lights it.
    Ready,
    /// Nothing to scatter. A sky is a lit atmosphere, so with no sun there is no
    /// sky and no pass — which is also what makes an indoor or space scene pay
    /// nothing for the feature existing.
    NoDirectionalLight,
    /// A sun, but nobody asked for a sky. Placing a light must not conjure an
    /// atmosphere around it; the Skybox component is the opt-in.
    NoSkybox,
    /// More than one sun. Unsupported rather than guessed at: with two the
    /// shadowed sun and the drawn sun could disagree, and a sky lit from one
    /// direction over shadows falling from another is a worse answer than none.
    MultipleDirectionalLights,
};

/// @brief Which celestial body is lighting the world this frame.
enum class LightingBody : std::uint8_t
{
    /// Neither. The sun is down and there is no moon up — polar night, or a
    /// level with no sun at all. A steady state rather than a transient: the
    /// scene is lit by the sky's own night floor and casts no sun shadows, which
    /// is the same path a scene with no directional light already takes.
    None,
    Sun,
    Moon,
};

/// @brief The one directional light the world is lit by, after the clock has had
/// its say.
///
/// One light, not two. The sun and the moon hand the slot between them at the
/// horizon, where both their intensities are exactly zero, so the swap is
/// invisible without any crossfade — and a second live cascade set is never paid
/// for.
struct CelestialLight
{
    /// Which entity's DirectionalLight row this replaces. Null when nothing lights.
    ECS::Entity entity = ECS::NullEntity;
    /// The direction the light TRAVELS, the way DirectionalLight stores one.
    glm::vec3 direction{0.f, -1.f, 0.f};
    /// Already through the atmosphere when the light is tinted by its sky.
    glm::vec3 color{1.f};
    /// Already through the horizon ramp, and through the moon's phase when the
    /// moon holds the slot. Zero at the handoff, from both sides.
    float intensity = 0.f;
    /// The light's own flag, and something to cast: a body at zero intensity
    /// draws cascades nothing can see.
    bool castsShadows = false;
    LightingBody body = LightingBody::None;
};

/// @brief What ResolveSky found.
///
/// @ref sun, @ref moon and @ref settings are meaningful when @ref status is
/// Ready. @ref light is meaningful when the status is Ready **or** NoSkybox — a
/// clocked sun over a level with no atmosphere is a legitimate thing to author,
/// and it still has to light the world.
struct SkyResolution
{
    SkyStatus status = SkyStatus::NoDirectionalLight;
    Render::SkySun sun;
    Render::SkyMoon moon;
    Render::SkySettings settings;

    CelestialLight light;

    /// @name What the clock came to, for panels and for consumers that budget ahead
    ///
    /// The angular velocities are here because a system that has to plan for the
    /// sun's motion — probe relighting, say — needs to know how fast it is going
    /// without re-deriving the clock. The shadow cadence deliberately does NOT
    /// read them: it measures what actually turned, which includes the camera and
    /// an editor scrub with no clock running at all.
    /// @{
    float sunAngularVelocity = 0.f;
    float moonAngularVelocity = 0.f;
    float moonLitFraction = 0.f;
    float declinationDegrees = 0.f;
    float daylightHours = 12.f;
    /// @}

    /// The least the sky may light the world, already scaled by its intensity.
    /// Black is off, which is the default.
    ///
    /// Kept here rather than in Render::SkySettings on purpose: SkySettings
    /// describes an atmosphere physically, and this is a playability floor. It
    /// applies where the indirect provider is chosen — see ResolveIndirect —
    /// which is also the only place that could honour it without the sky itself
    /// having to know about it.
    glm::vec3 minimumAmbient{0.f};

    /// How many times the clock has been cut. A change means forget any history
    /// keyed to the old sun direction.
    std::uint32_t jumpSerial = 0;

    double hour = 12.0;
    std::int32_t day = 0;
    double dayOfYear = 0.0;
};

/// @brief Find the scene's sky: exactly one directional light, carrying a Skybox.
///
/// Pure and device-free, which is the point — the rules above are the feature's
/// whole contract with a level, and they are worth testing without a GPU.
///
/// **Where the bodies are is derived here, every frame, and written nowhere.**
/// Nothing integrates a direction into DirectionalLight::direction, and that is
/// what makes the first frame after a load or a time jump correct with no tick in
/// between — and what lets the editor scrub the clock with play stopped, where no
/// fixed update is running to integrate anything.
///
/// Without a Sun component the light is aimed as authored, reversed: the
/// component stores where the light goes, and a sky is described by where the sun
/// is.
[[nodiscard]] SkyResolution ResolveSky(ECS::Scene &scene);

} // namespace Assisi::Runtime
