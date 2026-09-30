/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SoundDemo.hpp
/// @brief A sound placed in a level, played once when it has loaded.
///
/// Shows that a sound is an asset like any other: the level stores its id, the
/// asset store loads it in the background, and the mixer plays it.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// A sound to play once, on the SFX bus, as soon as it has loaded. A missing or
/// broken sound logs one warning and never plays.
ACOMP()
struct SoundDemo
{
    AFIELD() Assisi::Core::AssetId clip;

    AFIELD(transient) bool played = false;
};

/// Plays every SoundDemo whose sound has loaded and has not played yet. Does
/// nothing in a host with no audio device.
ASYSTEM(Update, name = "SoundDemo") void SoundDemoSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
