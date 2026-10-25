/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Bell.hpp
/// @brief A bell any player rings for everyone: the book's example of sending a
///        message and reading one.
///
/// A player presses B, which asks the host to ring the bell; the host tells
/// every machine it rang, and each plays its own bell's sound. Played alone the
/// same three systems ring it, since a world played alone is a host nobody
/// joined.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>

namespace Game
{

/// @brief A bell, which plays its entity's AudioEmitter when it rings. Every
/// machine has its own copy from the level, so it does not replicate.
ACOMP()
struct Bell
{
    /// Which bell this is, so a message can name it on every machine.
    AFIELD() int32_t number = 0;
};

/// @brief A player asking the host to ring bell @p bell.
AMSG(intent, reliable)
struct RingBell
{
    AFIELD() int32_t bell = 0;
};

/// @brief The host telling every machine that bell @p bell rang.
AMSG(event, reliable, independent)
struct BellRang
{
    AFIELD() int32_t bell = 0;
};

/// B asks the host to ring bell 1. `activeWorldOnly`, and it null-checks input:
/// a headless host has no keyboard.
ASYSTEM(Update, name = "AskForBell", activeWorldOnly)
void AskForBellSystem(Assisi::App::SystemContext &ctx);

/// The host rings every bell it is asked to, by telling everyone.
ASYSTEM(Update, name = "RingBells")
void RingBellsSystem(Assisi::App::SystemContext &ctx);

/// Every machine plays the bell it is told rang.
ASYSTEM(Update, name = "PlayBells")
void PlayBellsSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
