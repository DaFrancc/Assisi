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

/// @brief One attached sound. Once it finishes its slot is reused, and this
/// handle reads as finished from then on.
struct SoundHandle
{
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] bool operator==(const SoundHandle &) const = default;
};

/// @brief Starts, stops and tracks sounds on the mixer's buses.
///
/// Bus volumes are deliberately absent: they belong to the player's settings,
/// and a second way to set them would overwrite the player's choice. Every
/// method is called from the game thread.
class SoundOutput
{
public:
    virtual ~SoundOutput() = default;

    [[nodiscard]] virtual std::optional<BusId> FindBus(std::string_view name) const = 0;

    /// @brief Start @p clip on @p bus. The clip is held until the sound finishes.
    [[nodiscard]] virtual std::expected<SoundHandle, AudioError> Attach(std::shared_ptr<const PcmClip> clip,
                                                                        BusId bus) = 0;

    /// @brief Fade @p sound out. Does nothing to a finished sound.
    virtual void Stop(SoundHandle sound) = 0;

    [[nodiscard]] virtual bool IsFinished(SoundHandle sound) const = 0;
};

} // namespace Assisi::Audio
