/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SkyResolve.hpp
/// @brief Which sky a scene has, if any.

#include <Assisi/ECS/Scene.hpp>
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

/// @brief What ResolveSky found. @ref sun and @ref settings are meaningful only
/// when @ref status is Ready.
struct SkyResolution
{
    SkyStatus status = SkyStatus::NoDirectionalLight;
    Render::SkySun sun;
    Render::SkySettings settings;
};

/// @brief Find the scene's sky: exactly one directional light, carrying a Skybox.
///
/// Pure and device-free, which is the point — the rules above are the feature's
/// whole contract with a level, and they are worth testing without a GPU.
///
/// The sun's direction is taken from the light and reversed: DirectionalLight
/// stores the direction light TRAVELS, and everything about a sky is expressed
/// in terms of where the sun IS.
[[nodiscard]] SkyResolution ResolveSky(ECS::Scene &scene);

} // namespace Assisi::Runtime
