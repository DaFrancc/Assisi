/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioContextImpl.hpp
/// @brief The backend behind AudioContext, shared with the devices opened on it.

#include <Assisi/Audio/AudioContext.hpp>
#include <Assisi/Audio/AudioDeviceInfo.hpp>

#include <miniaudio.h>

#include <cstring>

namespace Assisi::Audio
{

static_assert(sizeof(ma_device_id) <= kDeviceIdBytes, "DeviceId must hold every backend's device id");

struct AudioContext::Impl
{
    /// Never moved once initialised: miniaudio devices keep a pointer to it.
    ma_context context{};
    bool initialised = false;

    Impl()                        = default;
    Impl(const Impl &)            = delete;
    Impl &operator=(const Impl &) = delete;
    ~Impl()
    {
        if (initialised)
        {
            ma_context_uninit(&context);
        }
    }
};

[[nodiscard]] inline DeviceId FromMaDeviceId(const ma_device_id &id) noexcept
{
    DeviceId out;
    std::memcpy(out.bytes.data(), &id, sizeof(id));
    return out;
}

[[nodiscard]] inline ma_device_id ToMaDeviceId(const DeviceId &id) noexcept
{
    ma_device_id out{};
    std::memcpy(&out, id.bytes.data(), sizeof(out));
    return out;
}

} // namespace Assisi::Audio
