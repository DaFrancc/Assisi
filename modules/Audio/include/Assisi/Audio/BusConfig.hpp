/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BusConfig.hpp
/// @brief The buses a game declares beside the defaults: a `config/buses.json` document.

#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <string_view>
#include <vector>

namespace Assisi::Audio
{

/// @brief Where the game's bus declarations live.
inline constexpr std::string_view kBusConfigPath = "config/buses.json";

ASTRUCT()
struct BusDeclaration
{
    AFIELD() Assisi::Core::InternedString name;

    /// A default bus, or a bus declared earlier in the list.
    AFIELD() Assisi::Core::InternedString parent;

    /// The volume the bus starts at before any player setting, 0 to 1.
    AFIELD() float volume = 1.0f;
};

AASSET()
struct BusConfig
{
    AFIELD() std::vector<BusDeclaration> buses;
};

} // namespace Assisi::Audio
