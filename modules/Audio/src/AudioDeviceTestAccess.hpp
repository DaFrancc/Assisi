/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioDeviceTestAccess.hpp
/// @brief What a test needs from a device that no backend a test runs on can show:
/// losing it, and how much the backend asked of it.

#include <cstdint>

namespace Assisi::Audio
{

class AudioDevice;

struct AudioDeviceTestAccess
{
    /// @brief Report the device lost exactly as the loss detectors in Update do.
    static void MarkLost(AudioDevice &device);

    /// @brief Every frame the backend has asked the device for since it opened.
    [[nodiscard]] static std::uint64_t FramesRequested(const AudioDevice &device);
};

} // namespace Assisi::Audio
