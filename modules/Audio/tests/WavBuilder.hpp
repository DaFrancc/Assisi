/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WavBuilder.hpp
/// @brief Writes a 16-bit PCM WAV file into memory, independently of the decoder
/// under test, so a decode test checks against bytes the decoder did not produce.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Assisi::Audio::Testing
{

/// @brief Append the low @p width bytes of @p value, little-endian.
inline void PutLittleEndian(std::vector<std::byte> &bytes, std::uint32_t value, std::uint32_t width)
{
    constexpr std::uint32_t kBitsPerByte = 8;
    constexpr std::uint32_t kByteMask    = 0xFF;
    for (std::uint32_t i = 0; i < width; ++i)
    {
        bytes.push_back(static_cast<std::byte>((value >> (i * kBitsPerByte)) & kByteMask));
    }
}

inline void PutTag(std::vector<std::byte> &bytes, std::string_view fourcc)
{
    for (const char c : fourcc)
    {
        bytes.push_back(static_cast<std::byte>(c));
    }
}

/// @brief A canonical RIFF/WAVE file: a RIFF header, a 16-byte fmt chunk and a
/// data chunk holding @p samples, interleaved.
inline std::vector<std::byte> BuildWav(std::span<const std::int16_t> samples, std::uint32_t sampleRate,
                                       std::uint16_t channels)
{
    constexpr std::uint16_t kPcmFormatTag     = 1;
    constexpr std::uint16_t kBitsPerSample    = 16;
    constexpr std::uint32_t kFmtChunkSize     = 16;
    constexpr std::uint32_t kWaveTagBytes     = 4;
    constexpr std::uint32_t kChunkHeaderBytes = 8;

    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * sizeof(std::int16_t));
    const std::uint32_t dataBytes  = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riffSize = kWaveTagBytes + kChunkHeaderBytes + kFmtChunkSize + kChunkHeaderBytes + dataBytes;

    std::vector<std::byte> bytes;
    PutTag(bytes, "RIFF");
    PutLittleEndian(bytes, riffSize, 4);
    PutTag(bytes, "WAVE");
    PutTag(bytes, "fmt ");
    PutLittleEndian(bytes, kFmtChunkSize, 4);
    PutLittleEndian(bytes, kPcmFormatTag, 2);
    PutLittleEndian(bytes, channels, 2);
    PutLittleEndian(bytes, sampleRate, 4);
    PutLittleEndian(bytes, sampleRate * blockAlign, 4);
    PutLittleEndian(bytes, blockAlign, 2);
    PutLittleEndian(bytes, kBitsPerSample, 2);
    PutTag(bytes, "data");
    PutLittleEndian(bytes, dataBytes, 4);
    for (const std::int16_t sample : samples)
    {
        PutLittleEndian(bytes, static_cast<std::uint16_t>(sample), 2);
    }
    return bytes;
}

} // namespace Assisi::Audio::Testing
