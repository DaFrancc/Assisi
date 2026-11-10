/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WaterComponents.hpp
/// @brief Open water: the endless Ocean, and the Lake within its rectangle.
///
/// Each puts water at its entity's height, with waves the level's Wind raises
/// over it. The components differ in how the water behaves: an ocean also
/// carries swell from storms far away, and a lake's size limits how far the
/// wind blows over it. A lake's preset says what kind of water it is: a
/// sheltered pond, open water, a clear mountain lake or a still swamp.
///
/// Not replicable: water is level data, which every machine reads from the
/// same file. The waves depend only on these fields and the shared clock, so
/// every machine draws the same ones.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Water.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace Assisi::Runtime
{

/// @name What water starts with
/// @{
inline constexpr float kDefaultWaterSpread = 0.2f;
inline constexpr float kDefaultSwellHeight = 1.f;
inline constexpr float kDefaultSwellBearingDegrees = 60.f;
inline constexpr float kDefaultOceanChoppiness = 1.f;
/// Open sea is clear for tens of metres.
inline constexpr float kDefaultOceanClarity = 12.f;
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

/// @brief A named kind of lake.
AENUM()
enum class LakePreset : std::uint8_t
{
    /// Small water sheltered by trees and banks: gusts cross it between calm
    /// spells, its ripples are soft, and it is murky green.
    Pond,
    /// Open water the wind reaches: natural ripples, bigger waves, clouded
    /// with silt.
    Lake,
    /// Clear and cold, partly sheltered by the slopes around it: the stones
    /// show metres down.
    MountainLake,
    /// Still brown water under a film: hardly any wind reaches it and hardly
    /// any ripples form.
    Swamp,
    /// The fields below, as authored.
    Custom,
};

/// @brief Water within a rectangle at its entity's height, turned with it: a
/// lake, or a pond.
ACOMP()
struct Lake
{
    /// Which kind of lake this is. Every value but Custom answers the nine
    /// fields from the colour to the swell, which grey out. The size and the
    /// seed are never greyed: they work with every preset.
    AFIELD(radioBroadcast) LakePreset preset = LakePreset::Lake;

    /// The colour light comes back from deep water in.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> color = Assisi::Render::kDefaultLakeColor;
    /// The colour of light that has crossed little water, which crests show
    /// when the sun is behind them.
    AFIELD(radioListen = {source = preset, value = Custom, behavior = grey}) Assisi::Math::Color3<Assisi::Math::ColorSpace::Linear> crestColor = Assisi::Render::kDefaultLakeCrestColor;
    /// From 0, waves travel close to the wind, to 1, every way equally.
    AFIELD(min = 0, max = 1, radioListen = {source = preset, value = Custom, behavior = grey}) float spread = Assisi::Render::kDefaultLakeSpread;
    /// How far the surface is pulled toward each crest, which sharpens the
    /// crests and flattens the troughs.
    AFIELD(min = 0, max = 2, radioListen = {source = preset, value = Custom, behavior = grey}) float choppiness = Assisi::Render::kDefaultLakeChoppiness;
    /// How far light goes into the water before all but a third of it is
    /// gone, in metres. Shallower than this the ground shows through.
    AFIELD(min = 0.1, radioListen = {source = preset, value = Custom, behavior = grey}) float clarity = Assisi::Render::kDefaultLakeClarity;
    /// How much of the level's wind the trees and banks around it keep off the
    /// water: 0 none, 1 all of it. Water stays calm for about fifty tree
    /// heights downwind of a tree line.
    AFIELD(min = 0, max = 1, radioListen = {source = preset, value = Custom, behavior = grey}) float shelter = Assisi::Render::kDefaultLakeShelter;
    /// How strongly its ripples show: 0 glassy, as under a film of oil or
    /// pollen, 1 as the wind raises them, 2 choppy.
    AFIELD(min = 0, max = 2, radioListen = {source = preset, value = Custom, behavior = grey}) float ripples = Assisi::Render::kDefaultLakeRipples;
    /// The height of the long, low waves that keep it rocking whatever the
    /// wind, in metres. 0 leaves it still when the air is.
    AFIELD(min = 0, max = 0.5, radioListen = {source = preset, value = Custom, behavior = grey}) float swellHeight = Assisi::Render::kDefaultLakeSwellHeight;
    /// Seconds between the swell's crests, from 1 to 4: longer swell is longer
    /// and slower.
    AFIELD(min = 1, max = 4, radioListen = {source = preset, value = Custom, behavior = grey}) float swellPeriod = Assisi::Render::kDefaultLakeSwellPeriod;

    /// Width along the entity's x and z, in metres, scaled with it. The wind
    /// can blow only this far over the water, so a small lake keeps only the
    /// short waves, and its waves grow across it from the shore upwind.
    AFIELD() glm::vec2 size{40.f, 40.f};
    /// Which lake of its kind: the same settings with another seed give other
    /// waves.
    AFIELD() uint32_t seed = 0u;
};

/// @brief Every preset, already built, in the order @ref LakePreset declares
/// them.
///
/// Each is written as what it changes from an open lake. The fields left out
/// are LakeSettings' own defaults, so a preset stays a complete answer:
/// switching from Swamp to Pond brings the pond's colour as well as its
/// ripples.
inline constexpr std::array<Assisi::Render::LakeSettings, static_cast<std::size_t>(LakePreset::Custom) + 1>
kLakePresets{{
    // Pond: most of the wind kept off, soft ripples, a faint slow swell, and
    // murky green water a metre and a half clear.
    Assisi::Render::LakeSettings{.color = glm::vec3(0.008f, 0.018f, 0.01f),
                                 .crestColor = glm::vec3(0.03f, 0.08f, 0.04f),
                                 .spread = 0.3f,
                                 .choppiness = 0.4f,
                                 .clarity = 1.5f,
                                 .shelter = 0.7f,
                                 .ripples = 0.7f,
                                 .swellHeight = 0.015f,
                                 .swellPeriod = 1.4f},

    // Lake: the defaults.
    Assisi::Render::LakeSettings{},

    // Mountain lake: clear to ten metres and deep blue, with the slopes keeping
    // some of the wind off.
    Assisi::Render::LakeSettings{.color = glm::vec3(0.002f, 0.012f, 0.03f),
                                 .crestColor = glm::vec3(0.02f, 0.12f, 0.16f),
                                 .clarity = 10.0f,
                                 .shelter = 0.6f,
                                 .swellHeight = 0.02f,
                                 .swellPeriod = 1.8f},

    // Swamp: brown and half a metre clear, with almost no wind, few ripples
    // and hardly any swell.
    Assisi::Render::LakeSettings{.color = glm::vec3(0.012f, 0.01f, 0.004f),
                                 .crestColor = glm::vec3(0.06f, 0.05f, 0.02f),
                                 .spread = 0.4f,
                                 .choppiness = 0.2f,
                                 .clarity = 0.5f,
                                 .shelter = 0.85f,
                                 .ripples = 0.25f,
                                 .swellHeight = 0.005f,
                                 .swellPeriod = 1.6f},

    // Custom: the defaults again, so departing from a preset starts where the
    // fields already are.
    Assisi::Render::LakeSettings{},
}};

static_assert(kLakePresets.size() == static_cast<std::size_t>(LakePreset::Custom) + 1,
              "Every LakePreset needs a row in kLakePresets, and Custom has to stay last.");

/// @brief The component's lake, as the render side wants it.
[[nodiscard]] inline Assisi::Render::LakeSettings ToLakeSettings(const Lake &lake)
{
    Assisi::Render::LakeSettings settings = kLakePresets[static_cast<std::size_t>(lake.preset)];
    if (lake.preset == LakePreset::Custom)
    {
        settings.color = lake.color;
        settings.crestColor = lake.crestColor;
        settings.spread = lake.spread;
        settings.choppiness = lake.choppiness;
        settings.clarity = lake.clarity;
        settings.shelter = lake.shelter;
        settings.ripples = lake.ripples;
        settings.swellHeight = lake.swellHeight;
        settings.swellPeriod = lake.swellPeriod;
    }
    settings.seed = lake.seed;
    return Assisi::Render::Sanitized(settings);
}

} // namespace Assisi::Runtime
