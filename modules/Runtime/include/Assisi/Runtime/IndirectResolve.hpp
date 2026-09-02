/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file IndirectResolve.hpp
/// @brief Which indirect-lighting provider answers for a scene.

#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/IndirectLighting.hpp>
#include <Assisi/Runtime/SkyResolve.hpp>

namespace Assisi::Runtime
{

/// @brief An indirect term pinned by hand, overriding whatever the scene would
/// otherwise be lit by.
///
/// What an interior wants — a room is not lit by a sky it cannot see — and what
/// the model viewer turns up to make a mesh visible without anyone lighting one.
struct AmbientOverride
{
    /// False leaves the scene to answer for itself. The colour and intensity are
    /// still read in that case, because a scene with no sky has nothing else to
    /// be lit by and these are what it had before.
    bool active = false;
    Assisi::Math::Color3 color{1.0f};
    float intensity = Render::kDefaultAmbientIntensity;
};

/// @brief The indirect term for one frame, as the shader wants it.
///
/// The one place a concrete provider is named. Everything downstream — the mesh
/// pass, its constant buffer, mesh.frag — reads Render::IndirectConstants and
/// knows nothing about which provider produced them, so a baked or probe-based
/// provider arrives here and nowhere else.
///
/// A pinned ambient wins over the sky rather than adding to it: an author who
/// says what the indirect term is has answered the question, and a sky arriving
/// on top of that answer would be a second one.
[[nodiscard]] inline Render::IndirectConstants ResolveIndirect(const SkyResolution &sky,
                                                               const AmbientOverride &ambient)
{
    if (!ambient.active && sky.status == SkyStatus::Ready)
    {
        const Render::SkyAmbient fromSky = Render::AmbientFromSky(sky.sun, sky.settings);
        return Render::HemisphereIndirect(fromSky.sky, fromSky.ground).ShaderConstants();
    }
    return Render::UniformIndirect(ambient.color, ambient.intensity).ShaderConstants();
}

} // namespace Assisi::Runtime
