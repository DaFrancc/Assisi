/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioDevice.hpp
/// @brief The output device: the one piece of the engine that talks to audio hardware.
///
/// The backend is hidden behind a pimpl, so no consumer includes its header.

#include <Assisi/Audio/AudioContext.hpp>
#include <Assisi/Audio/AudioDeviceInfo.hpp>
#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/AudioRenderer.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

namespace Assisi::Audio
{

enum class DeviceState : std::uint8_t
{
    Stopped, ///< Open and silent: never started, or stopped on request.
    Running, ///< Calling its renderer.
    Lost,    ///< Stopped by the hardware going away; Update is reopening on the default device.
    Count,
};

/// @brief An open output device playing the engine format (AudioFormat.hpp).
///
/// Threading: every method is called from the game thread. Once started, the
/// backend calls the renderer from its own audio thread until Stop or
/// destruction.
///
/// Losing the device — it is unplugged, or disabled by the system — is noticed
/// and repaired by Update, which reopens on the system default device and
/// resumes the same renderer. Until then the output is silent.
class AudioDevice
{
public:
    struct Impl;

    /// @brief Open @p chosen on @p context's backend, or the system default
    /// device when @p chosen is empty or no longer listed. The device is silent
    /// until Start.
    [[nodiscard]] static std::expected<AudioDevice, AudioError> Open(const AudioContext &context,
                                                                     std::optional<DeviceId> chosen);

    AudioDevice(AudioDevice &&) noexcept;
    AudioDevice &operator=(AudioDevice &&) noexcept;
    AudioDevice(const AudioDevice &)            = delete;
    AudioDevice &operator=(const AudioDevice &) = delete;
    ~AudioDevice();

    /// @brief Start pulling samples from @p renderer, which must outlive the
    /// running period (AudioRenderer.hpp). A failure leaves the device Lost, so
    /// the next Update tries the default device.
    [[nodiscard]] std::expected<void, AudioError> Start(AudioRenderer &renderer);

    /// @brief Stop the callback. Once this returns, the renderer is never called
    /// again. Does nothing on a device that is not running.
    void Stop() noexcept;

    /// @brief Notice a lost device and reopen on the default one. Call once per
    /// frame; costs a few atomic loads and a clock read while the device is healthy.
    void Update();

    [[nodiscard]] DeviceState State() const noexcept;

    /// @brief The device asked for at Open, until a loss moves playback to the
    /// default device.
    [[nodiscard]] std::optional<DeviceId> ChosenDevice() const noexcept;

    /// @brief The name of the device playing now, for a log line or a settings menu.
    [[nodiscard]] std::string_view CurrentDeviceName() const noexcept;

private:
    friend struct AudioDeviceTestAccess;

    explicit AudioDevice(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> _impl;
};

} // namespace Assisi::Audio
