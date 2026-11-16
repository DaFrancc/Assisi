/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WindComponents.hpp
/// @brief The level's wind, which the clouds and the water move with.
///
/// One Wind per level, on any entity; a level without one has still air.
/// Replicable, so a game that changes the wind during play can send it.

#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Assisi::Runtime
{

/// @name The wind a new Wind starts with, and its limit
/// @{
/// A fresh breeze, toward the east.
inline constexpr float kDefaultWindSpeed = 8.f;
inline constexpr float kDefaultWindBearingDegrees = 90.f;
/// The strongest wind anything is built for, in metres per second. Hurricane
/// force begins at about 33.
inline constexpr float kMaxWindSpeed = 40.f;
/// Between open land's 1.66 and a coast's 1.38 to 1.52, measured as the peak
/// three-second gust over the ten-minute average.
inline constexpr float kDefaultGustFactor = 1.5f;
/// A steady wind, with no gusts.
inline constexpr float kSteadyGustFactor = 1.f;
/// Gustier than any measured site's average.
inline constexpr float kMaxGustFactor = 3.f;
/// @}

/// @brief The wind across the whole level.
ACOMP(replicable)
struct Wind
{
    /// Metres per second: the average over ten minutes, 10 m above the ground,
    /// as a forecast gives it. A gale is about twenty.
    AFIELD(min = 0, max = 40) float speed = kDefaultWindSpeed;
    /// The compass bearing it blows toward: 0 north, 90 east. A forecast gives
    /// the bearing it blows from, the opposite one.
    AFIELD() float bearingDegrees = kDefaultWindBearingDegrees;
    /// The strongest gusts over the steady speed: 1.5 gusts to half again as
    /// fast, and lulls to half as fast. 1 is a steady wind. The gusts drift
    /// across the level with the wind.
    AFIELD(min = 1, max = 3) float gustFactor = kDefaultGustFactor;
};

/// @brief The air in a level with no Wind.
inline constexpr Wind kStillAir{.speed = 0.f, .bearingDegrees = 0.f, .gustFactor = kSteadyGustFactor};

} // namespace Assisi::Runtime
