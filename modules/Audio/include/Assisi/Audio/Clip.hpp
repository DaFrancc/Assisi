/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Clip.hpp
/// @brief A sound decoded into memory, and decoding one from an encoded file.

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/AudioFormat.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace Assisi::Audio
{

/// @brief Interleaved samples in the engine format (AudioFormat.hpp).
struct PcmClip
{
    std::vector<float> samples;

    [[nodiscard]] std::uint64_t Frames() const noexcept { return FrameCount(samples.size()); }
};

/// @brief Decode a WAV, FLAC or MP3 file held in memory into the engine format.
///
/// The source's rate and channel count are converted here, once, so nothing
/// downstream ever resamples or remixes a clip.
[[nodiscard]] std::expected<PcmClip, AudioError> DecodeClip(std::span<const std::byte> encoded);

} // namespace Assisi::Audio
