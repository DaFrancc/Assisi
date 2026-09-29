/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "AudioContextImpl.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace Assisi::Audio
{

AudioContext::AudioContext(std::shared_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}

std::expected<AudioContext, AudioError> AudioContext::Create(AudioBackend backend)
{
    std::shared_ptr<Impl> impl = std::make_shared<Impl>();
    const ma_context_config config = ma_context_config_init();

    // An empty list asks miniaudio for its own priority order.
    std::array<ma_backend, 1> nullOnly{ma_backend_null};
    std::span<ma_backend> backends;
    if (backend == AudioBackend::Null)
    {
        backends = nullOnly;
    }

    if (ma_context_init(backends.empty() ? nullptr : backends.data(), static_cast<ma_uint32>(backends.size()),
                        &config, &impl->context) != MA_SUCCESS)
    {
        return std::unexpected(AudioError::BackendUnavailable);
    }
    impl->initialised = true;
    return AudioContext(std::move(impl));
}

std::expected<std::vector<DeviceInfo>, AudioError> AudioContext::ListDevices(DeviceKind kind) const
{
    ma_device_info *playback    = nullptr;
    ma_uint32 playbackCount     = 0;
    ma_device_info *capture     = nullptr;
    ma_uint32 captureCount      = 0;
    if (ma_context_get_devices(&_impl->context, &playback, &playbackCount, &capture, &captureCount) != MA_SUCCESS)
    {
        return std::unexpected(AudioError::EnumerationFailed);
    }

    // The backend's array is reused by the next enumeration, so everything is copied out now.
    const std::span<const ma_device_info> listed = kind == DeviceKind::Output
                                                       ? std::span<const ma_device_info>(playback, playbackCount)
                                                       : std::span<const ma_device_info>(capture, captureCount);
    std::vector<DeviceInfo> devices;
    devices.reserve(listed.size());
    for (const ma_device_info &info : listed)
    {
        devices.push_back(DeviceInfo{
                .id        = FromMaDeviceId(info.id),
                .name      = info.name,
                .isDefault = info.isDefault != MA_FALSE,
            });
    }
    return devices;
}

std::string_view AudioContext::BackendName() const noexcept
{
    return ma_get_backend_name(_impl->context.backend);
}

} // namespace Assisi::Audio
