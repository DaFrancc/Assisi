/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WaterComponents.hpp
/// @brief Open water: the endless Ocean, and the Lake within its rectangle.
///
/// Each puts water at its entity's height, with waves the level's Wind raises
/// over it. The components differ in how the water behaves: an ocean also
/// carries swell from storms far away, and a lake's size limits how far the
/// wind blows over it, so a small lake, a pond, gets small waves without being
/// told to.
///
/// Not replicable: water is level data, which every machine reads from the
/// same file. The waves depend only on these fields and the shared clock, so
/// every machine draws the same ones.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Water.hpp>

#include <cstdint>

namespace Assisi::Runtime
{

/// @name What water starts with
/// @{
inline constexpr float kDefaultWaterSpread = 0.2f;
inline constexpr float kDefaultSwellHeight = 1.f;
inline constexpr float kDefaultSwellBearingDegrees = 60.f;
inline constexpr float kDefaultOceanChoppiness = 1.f;
inline constexpr float kDefaultLakeChoppiness = 0.6f;
/// Open sea is clear for tens of metres; a lake is clouded with silt.
inline constexpr float kDefaultOceanClarity = 12.f;
inline constexpr float kDefaultLakeClarity = 4.f;
/// The least clarity a body may have, in metres.
inline constexpr float kMinWaterClarity = 0.1f;
/// @}

/// @brief Endless water at its entity's height: the sea.
ACOMP()
struct Ocean
{
    /// The colour light comes back from deep water in.
    AFIELD() Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> color{0.004f, 0.016f, 0.026f};
    /// The colour of light that has crossed little water, which crests show
    /// when the sun is behind them.
    AFIELD() Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> crestColor{0.03f, 0.16f, 0.15f};
    /// From 0, waves travel close to the wind, to 1, every way equally.
    AFIELD(min = 0, max = 1) float spread = kDefaultWaterSpread;
    /// The swell's significant height, in metres, as a forecast gives it: about
    /// the height of its bigger waves from trough to crest. Zero is no swell.
    AFIELD(min = 0, max = 10) float swellHeight = kDefaultSwellHeight;
    /// Seconds between the swell's crests: longer swell has come further.
    AFIELD(min = 4, max = 25) float swellPeriod = Assisi::Render::kDefaultSwellPeriod;
    /// The compass bearing the swell travels toward.
    AFIELD() float swellBearingDegrees = kDefaultSwellBearingDegrees;
    /// How far the surface is pulled toward each crest, which sharpens the
    /// crests and flattens the troughs. Zero moves it only up and down.
    AFIELD(min = 0, max = 2) float choppiness = kDefaultOceanChoppiness;
    /// How far light goes into the water before all but a third of it is
    /// gone, in metres. Shallower than this the ground shows through.
    AFIELD(min = 0.1) float clarity = kDefaultOceanClarity;
    /// Which sea of its kind: the same settings with another seed give other
    /// waves.
    AFIELD() uint32_t seed = 0u;
};

/// @brief Water within a rectangle at its entity's height, turned with it: a
/// lake, or a pond.
ACOMP()
struct Lake
{
    /// The same as an Ocean's.
    AFIELD() Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> color{0.006f, 0.02f, 0.016f};
    AFIELD() Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> crestColor{0.03f, 0.1f, 0.06f};
    /// Width along the entity's x and z, in metres, scaled with it. The wind
    /// can blow only this far over the water, which keeps a small lake's waves
    /// small.
    AFIELD() glm::vec2 size{40.f, 40.f};
    AFIELD(min = 0, max = 1) float spread = kDefaultWaterSpread;
    AFIELD(min = 0, max = 2) float choppiness = kDefaultLakeChoppiness;
    AFIELD(min = 0.1) float clarity = kDefaultLakeClarity;
    AFIELD() uint32_t seed = 0u;
};

} // namespace Assisi::Runtime
