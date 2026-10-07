/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioDeviceInfo.hpp
/// @brief What an AudioContext reports about one output or input device.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Assisi::Audio
{

enum class DeviceKind : std::uint8_t
{
    Output,
    Input,
    Count_,
};

/// @brief Room for the largest identifier any backend uses.
inline constexpr std::size_t kDeviceIdBytes = 256;

/// @brief Names one device to the backend that listed it.
///
/// Opaque, and meaningful only to a context on the same backend: anything that
/// stores one across runs must store the context's BackendName beside it.
struct DeviceId
{
    std::array<std::byte, kDeviceIdBytes> bytes{};

    [[nodiscard]] bool operator==(const DeviceId &) const = default;
};

struct DeviceInfo
{
    DeviceId id;
    std::string name;
    bool isDefault = false;
};

} // namespace Assisi::Audio
