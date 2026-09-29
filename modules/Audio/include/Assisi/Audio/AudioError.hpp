/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioError.hpp
/// @brief Why an audio device or decode operation failed.

#include <cstdint>
#include <string_view>

namespace Assisi::Audio
{

enum class AudioError : std::uint8_t
{
    BackendUnavailable,  ///< No backend of the kind asked for could be initialised.
    DeviceInitFailed,    ///< A backend came up but refused to open an output device.
    DeviceStartFailed,   ///< The device opened but would not start running its callback.
    AlreadyStarted,      ///< Start was called on a device that is already running.
    UnsupportedEncoding, ///< The bytes are not audio in any format this build decodes.
    DecodeFailed,        ///< The encoding was recognised but reading it failed part-way.
    EnumerationFailed,   ///< The backend could not list its devices.
    MixerInitFailed,     ///< The mixer's graph of buses could not be built.
    TooManySounds,       ///< Every sound slot in the mixer is playing.
    UnknownBus,          ///< A bus id the mixer's layout does not have.
    NoClip,              ///< A sound was attached with no clip to play.
    TooManyBuses,        ///< The bus config declares more buses than the mixer holds.
    DuplicateBus,        ///< The bus config names a bus twice, or redeclares a default bus.
    UnknownParentBus,    ///< A declared bus's parent is not a default bus or one declared before it.
    Count,
};

/// @brief A short human-readable description, for a log line.
[[nodiscard]] std::string_view ToString(AudioError error) noexcept;

} // namespace Assisi::Audio
