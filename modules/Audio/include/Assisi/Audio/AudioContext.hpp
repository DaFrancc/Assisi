/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioContext.hpp
/// @brief One audio backend, and the devices it can see.

#include <Assisi/Audio/AudioDeviceInfo.hpp>
#include <Assisi/Audio/AudioError.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

namespace Assisi::Audio
{

/// @brief Which backend a context runs on.
enum class AudioBackend : std::uint8_t
{
    Default, ///< The platform's best available backend.
    Null,    ///< No hardware: device callbacks run on a timer. For tests and headless runs.
    Count_,
};

/// @brief Lists devices and opens them (AudioDevice::Open).
///
/// A device shares ownership of its context's backend, so destroying the
/// context while a device is open is safe. Every method is called from the game
/// thread.
class AudioContext
{
public:
    struct Impl;

    [[nodiscard]] static std::expected<AudioContext, AudioError> Create(AudioBackend backend);

    /// @brief Every device of @p kind the backend can see right now.
    [[nodiscard]] std::expected<std::vector<DeviceInfo>, AudioError> ListDevices(DeviceKind kind) const;

    /// @brief The backend's name, for a log line.
    [[nodiscard]] std::string_view BackendName() const noexcept;

    /// @brief The backend, for AudioDevice to open on.
    [[nodiscard]] const std::shared_ptr<Impl> &Backend() const noexcept { return _impl; }

private:
    explicit AudioContext(std::shared_ptr<Impl> impl) noexcept;

    std::shared_ptr<Impl> _impl;
};

} // namespace Assisi::Audio
