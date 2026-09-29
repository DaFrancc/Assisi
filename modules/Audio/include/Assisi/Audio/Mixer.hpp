/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Mixer.hpp
/// @brief Sums every playing sound through a tree of buses, each with its own volume.

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/AudioFormat.hpp>
#include <Assisi/Audio/AudioRenderer.hpp>
#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Clip.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace Assisi::Audio
{

inline constexpr float kMinVolume = 0.0f;
inline constexpr float kMaxVolume = 1.0f;

/// @brief How long a bus takes to reach a new volume: long enough not to click,
/// short enough that a slider feels immediate.
inline constexpr std::uint32_t kVolumeRampFrames = kSampleRate / 50;

/// @brief How long a stopped sound takes to fade to silence rather than cut off.
inline constexpr std::uint32_t kSoundFadeFrames = kSampleRate / 100;

/// @brief Sounds that can play at once. Every slot is built up front, so
/// attaching a sound never allocates.
inline constexpr std::size_t kMaxSounds = 256;

/// @brief One attached sound. Once it finishes its slot is reused, and this
/// handle reads as finished from then on.
struct SoundHandle
{
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] bool operator==(const SoundHandle &) const = default;
};

/// @brief Mixes clips through buses into what an AudioDevice plays.
///
/// Threading: Render runs on the audio thread; every other method is called
/// from the game thread. Volume changes reach the audio thread through one
/// atomic per bus and ramp there; attaching and stopping sounds edit the
/// backend's graph, which is built to be edited while it plays.
///
/// Neither copied nor moved, so a device playing it can hold it by reference.
///
/// Attach is the building block emitters play through, not the way game code
/// plays a sound.
class Mixer final : public AudioRenderer
{
public:
    struct Impl;

    [[nodiscard]] static std::expected<std::unique_ptr<Mixer>, AudioError> Create(const BusLayout &layout);

    Mixer(const Mixer &)            = delete;
    Mixer &operator=(const Mixer &) = delete;
    Mixer(Mixer &&)                 = delete;
    Mixer &operator=(Mixer &&)      = delete;
    ~Mixer() override;

    /// @brief Ramp @p bus to @p volume, clamped to kMinVolume..kMaxVolume.
    void SetBusVolume(BusId bus, float volume);
    [[nodiscard]] float BusVolume(BusId bus) const;
    [[nodiscard]] const BusLayout &Layout() const noexcept;

    /// @brief Start @p clip on @p bus. The mixer holds the clip until the sound
    /// finishes and Update releases it.
    [[nodiscard]] std::expected<SoundHandle, AudioError> Attach(std::shared_ptr<const PcmClip> clip, BusId bus);

    /// @brief Fade @p sound out over kSoundFadeFrames. Does nothing to a finished sound.
    void Stop(SoundHandle sound);

    [[nodiscard]] bool IsFinished(SoundHandle sound) const;

    /// @brief Release every finished sound's slot and clip. Call once per frame.
    void Update();

    void Render(std::span<float> interleaved) noexcept override;

private:
    explicit Mixer(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> _impl;
};

} // namespace Assisi::Audio
