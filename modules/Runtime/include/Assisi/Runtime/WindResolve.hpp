/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WindResolve.hpp
/// @brief Which wind a scene has.

#include <Assisi/ECS/SceneFwd.hpp>
#include <Assisi/Runtime/WindComponents.hpp>

#include <cstdint>

namespace Assisi::Runtime
{

/// @brief What ResolveWind found.
struct WindResolution
{
    /// Finite and in range. Still air when the scene has no Wind.
    Wind wind = kStillAir;
    /// How many Winds the scene has. Past one, the first found is the wind.
    std::uint32_t count = 0;
};

/// @brief The scene's wind: its first Wind, or still air.
[[nodiscard]] WindResolution ResolveWind(const ECS::Scene &scene);

} // namespace Assisi::Runtime
