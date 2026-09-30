/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SoundOutput.hpp
/// @brief What systems play sounds through: the mixer, without its bus volumes.

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Clip.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace Assisi::Audio
{

/// @brief One claimed sound. Once it finishes its slot is reused, and this
/// handle reads as finished from then on.
struct SoundHandle
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] bool operator==(const SoundHandle &) const = default;
};

/// @brief Starts, stops and tracks sounds on the mixer's buses.
///
/// A sound is claimed, set up, then played: its volume, looping and paused
/// state are set between Claim and Play, so the first buffer it renders is
/// already right, and set again while it plays, when they ramp rather than jump.
///
/// Every claimed or playing sound must be held once between one Update and the
/// next, or it fades out and its slot is released: a sound lives exactly as long
/// as something keeps asking for it.
///
/// Bus volumes are deliberately absent: they belong to the player's settings,
/// and a second way to set them would overwrite the player's choice. Every
/// method is called from the game thread, and every method but Claim and
/// IsFinished does nothing to a finished sound.
class SoundOutput
{
  public:
    virtual ~SoundOutput() = default;

    [[nodiscard]] virtual std::optional<BusId> FindBus(std::string_view name) const = 0;

    /// @brief Reserve a slot for @p clip on @p bus, silent until Play. The clip is
    ///        kept until the sound finishes. A claim counts as held until the next Update.
    [[nodiscard]] virtual std::expected<SoundHandle, AudioError> Claim(std::shared_ptr<const PcmClip> clip,
                                                                       BusId bus) = 0;

    /// @brief Start a claimed sound. Does nothing to one already playing.
    virtual void Play(SoundHandle sound) = 0;

    /// @brief Set @p sound's own volume, clamped to kMinVolume..kMaxVolume, on top
    ///        of its bus's. Taken at once before Play, ramped after.
    virtual void SetVolume(SoundHandle sound, float volume) = 0;

    /// @brief Whether @p sound starts again from the beginning when it reaches the end.
    virtual void SetLooping(SoundHandle sound, bool looping) = 0;

    /// @brief Fade @p sound to silence where it is; it keeps its place and its slot.
    virtual void Pause(SoundHandle sound) = 0;

    /// @brief Fade a paused @p sound back in from where it paused.
    virtual void Resume(SoundHandle sound) = 0;

    /// @brief Keep @p sound alive through the next Update.
    virtual void Hold(SoundHandle sound) = 0;

    /// @brief Fade @p sound out and release it.
    virtual void Stop(SoundHandle sound) = 0;

    /// @brief True once @p sound has ended or been released, and for any handle
    ///        whose slot has since been reused.
    [[nodiscard]] virtual bool IsFinished(SoundHandle sound) const = 0;
};

} // namespace Assisi::Audio
