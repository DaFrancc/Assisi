/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Fixtures.hpp"
#include "WavBuilder.hpp"

#include <Assisi/Audio/AudioFormat.hpp>
#include <Assisi/Audio/Clip.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

using namespace Assisi::Audio;

namespace
{

/// @brief One step of 16-bit PCM in float, which bounds any honest conversion's error.
constexpr float kSampleTolerance = 1.0f / 32768.0f;

/// @brief A resampler may emit a few frames more or fewer at the edges than the
/// exact rate ratio; being off by a whole period would mean no resampling at all.
constexpr std::int64_t kResampleFrameTolerance = 64;

constexpr float kPcm16FullScale = 32768.0f;

} // namespace

TEST_CASE("DecodeClip widens mono to the engine's channel count, sample for sample")
{
    const std::vector<std::int16_t> source{0, 16384, -16384, 32767, -32768, 1000, -1000, 8};
    const std::vector<std::byte> wav = Testing::BuildWav(source, kSampleRate, 1);

    const std::expected<PcmClip, AudioError> clip = DecodeClip(wav);
    REQUIRE(clip.has_value());
    REQUIRE(clip->Frames() == source.size());

    for (std::size_t frame = 0; frame < source.size(); ++frame)
    {
        const float expected = static_cast<float>(source[frame]) / kPcm16FullScale;
        for (std::uint32_t channel = 0; channel < kChannelCount; ++channel)
        {
            CHECK(std::fabs(clip->samples[frame * kChannelCount + channel] - expected) <= kSampleTolerance);
        }
    }
}

TEST_CASE("DecodeClip resamples a clip recorded at another rate to the engine rate")
{
    constexpr std::uint32_t kHalfRate = kSampleRate / 2;
    constexpr std::size_t kSourceFrames = 4800;
    const std::vector<std::int16_t> source(kSourceFrames, 1000);
    const std::vector<std::byte> wav = Testing::BuildWav(source, kHalfRate, 1);

    const std::expected<PcmClip, AudioError> clip = DecodeClip(wav);
    REQUIRE(clip.has_value());

    const std::int64_t expectedFrames = static_cast<std::int64_t>(kSourceFrames) * 2;
    const std::int64_t frames = static_cast<std::int64_t>(clip->Frames());
    CHECK(std::llabs(frames - expectedFrames) <= kResampleFrameTolerance);
}

TEST_CASE("DecodeClip decodes the Ogg Vorbis and FLAC files a game ships")
{
    // Each fixture is a quarter-second 440 Hz tone recorded at the engine rate.
    constexpr std::uint64_t kToneFrames = kSampleRate / 4;
    // A lossy codec's encoder delay and padding move the length by a few blocks at most.
    constexpr std::int64_t kCodecFrameTolerance = 2048;
    // The fixtures' tone peaks near 0.09; a decode that produced silence stays near zero.
    constexpr float kAudibleLevel = 0.05f;

    for (const std::string_view name : {std::string_view{"tone.ogg"}, std::string_view{"tone.flac"}})
    {
        INFO(name);
        const std::vector<std::byte> encoded = Testing::ReadFixture(name);
        REQUIRE_FALSE(encoded.empty());

        const std::expected<PcmClip, AudioError> clip = DecodeClip(encoded);
        REQUIRE(clip.has_value());
        const std::int64_t frames = static_cast<std::int64_t>(clip->Frames());
        CHECK(std::llabs(frames - static_cast<std::int64_t>(kToneFrames)) <= kCodecFrameTolerance);

        float peak = 0.0f;
        for (const float sample : clip->samples)
        {
            peak = std::max(peak, std::fabs(sample));
        }
        CHECK(peak > kAudibleLevel);
    }
}

TEST_CASE("DecodeClip refuses MP3, which a game converts to Ogg Vorbis or FLAC instead")
{
    const std::vector<std::byte> encoded = Testing::ReadFixture("tone.mp3");
    REQUIRE_FALSE(encoded.empty());

    const std::expected<PcmClip, AudioError> clip = DecodeClip(encoded);
    REQUIRE_FALSE(clip.has_value());
    CHECK(clip.error() == AudioError::UnsupportedEncoding);
}

TEST_CASE("DecodeClip refuses bytes that are not audio")
{
    const std::array<std::byte, 64> noise{std::byte{0x13}, std::byte{0x37}, std::byte{0xC0}, std::byte{0xDE}};

    const std::expected<PcmClip, AudioError> clip = DecodeClip(noise);
    REQUIRE_FALSE(clip.has_value());
    CHECK(clip.error() == AudioError::UnsupportedEncoding);
}
