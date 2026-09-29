/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioFormat.hpp
/// @brief The one sample format every clip, mix and device in the engine uses.
///
/// Samples are 32-bit float, interleaved. The format is fixed rather than taken
/// from the device, so a decoded clip and a mix come out identical on every
/// machine; a device running at another rate is converted by the backend.

#include <cstddef>
#include <cstdint>

namespace Assisi::Audio
{

/// @brief Stereo: panning is the spatial bar, and no surround output is planned.
inline constexpr std::uint32_t kChannelCount = 2;

/// @brief The rate most output hardware runs at natively, so the backend
/// converts nothing on the common path.
inline constexpr std::uint32_t kSampleRate = 48000;

/// @brief How many frames @p sampleCount interleaved samples hold.
[[nodiscard]] constexpr std::uint64_t FrameCount(std::size_t sampleCount) noexcept
{
    return static_cast<std::uint64_t>(sampleCount) / kChannelCount;
}

} // namespace Assisi::Audio
