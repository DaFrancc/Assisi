/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioRenderer.hpp
/// @brief What a running AudioDevice pulls its samples from.

#include <span>

namespace Assisi::Audio
{

/// @brief Produces the samples a device plays.
///
/// Render runs on the backend's audio thread, not the game thread. It must not
/// take a lock, allocate, or wait on the job system: any of those can stall
/// past the device's deadline, and a late buffer is an audible glitch.
///
/// The renderer must outlive the device's running period — stop the device, or
/// destroy it, before the renderer goes away. An owner that holds both should
/// declare the device after the renderer, so the device is destroyed first.
class AudioRenderer
{
public:
    virtual ~AudioRenderer() = default;

    /// @brief Fill all of @p interleaved, whose length is a whole number of
    /// frames of kChannelCount samples each. Write zeros for silence.
    virtual void Render(std::span<float> interleaved) noexcept = 0;
};

} // namespace Assisi::Audio
