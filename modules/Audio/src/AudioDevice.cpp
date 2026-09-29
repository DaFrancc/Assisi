/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/AudioDevice.hpp>

#include "AudioContextImpl.hpp"
#include "AudioDeviceTestAccess.hpp"

#include <Assisi/Audio/AudioFormat.hpp>
#include <Assisi/Core/Logger.hpp>

#include <miniaudio.h>

#include <atomic>
#include <chrono>
#include <span>
#include <string>
#include <utility>

namespace Assisi::Audio
{

namespace
{

using Clock = std::chrono::steady_clock;

/// @brief How long a running device may go without calling back before it is
/// treated as lost. Far above any backend's period, so only a device that has
/// really stopped trips it; a stream pinned to a removed device can die without
/// the backend reporting anything, and this is what notices.
constexpr std::chrono::milliseconds kCallbackStallTimeout{2000};

/// @brief How often Update retries the default device while none will open.
constexpr std::chrono::milliseconds kReopenRetryInterval{1000};

} // namespace

struct AudioDevice::Impl
{
    /// Never moved: the backend holds its address and the callback finds this
    /// Impl through it.
    ma_device device{};
    std::shared_ptr<AudioContext::Impl> context;
    std::optional<DeviceId> chosen;
    std::string currentName;
    Clock::time_point lastProgress;
    Clock::time_point lastReopenAttempt;
    /// Frames the backend has asked for, so the game thread can see the device
    /// is alive. The only state the audio thread writes.
    std::atomic<std::uint64_t> framesRequested{0};
    std::uint64_t lastSeenFrames = 0;
    /// Read by the audio thread. Written only while the device is stopped;
    /// starting and stopping the device order those writes against the reads.
    AudioRenderer *renderer = nullptr;
    DeviceState state       = DeviceState::Stopped;
    bool deviceInitialised  = false;

    Impl()                        = default;
    Impl(const Impl &)            = delete;
    Impl &operator=(const Impl &) = delete;
    ~Impl() { CloseBackendDevice(); }

    [[nodiscard]] std::expected<void, AudioError> OpenBackendDevice(const std::optional<DeviceId> &id);
    void CloseBackendDevice() noexcept;
    [[nodiscard]] bool StartBackendDevice();
    void MarkLost() noexcept;
    void TryReopenOnDefault();
};

namespace
{

void OnData(ma_device *device, void *output, const void * /*input*/, ma_uint32 frameCount)
{
    AudioDevice::Impl *impl = static_cast<AudioDevice::Impl *>(device->pUserData);
    impl->framesRequested.fetch_add(frameCount, std::memory_order_relaxed);
    if (impl->renderer != nullptr)
    {
        // The backend hands over a zeroed buffer, so a device with no renderer plays silence.
        impl->renderer->Render(std::span<float>(static_cast<float *>(output), frameCount * kChannelCount));
    }
}

} // namespace

std::expected<void, AudioError> AudioDevice::Impl::OpenBackendDevice(const std::optional<DeviceId> &id)
{
    ma_device_config config  = ma_device_config_init(ma_device_type_playback);
    config.playback.format   = ma_format_f32;
    config.playback.channels = kChannelCount;
    config.sampleRate        = kSampleRate;
    config.dataCallback      = OnData;
    config.pUserData         = this;

    ma_device_id backendId{};
    if (id.has_value())
    {
        backendId              = ToMaDeviceId(*id);
        config.playback.pDeviceID = &backendId;
    }

    if (ma_device_init(&context->context, &config, &device) != MA_SUCCESS)
    {
        return std::unexpected(AudioError::DeviceInitFailed);
    }
    deviceInitialised = true;
    currentName       = device.playback.name;
    return {};
}

void AudioDevice::Impl::CloseBackendDevice() noexcept
{
    if (deviceInitialised)
    {
        ma_device_uninit(&device);
        deviceInitialised = false;
    }
}

bool AudioDevice::Impl::StartBackendDevice()
{
    lastSeenFrames = framesRequested.load(std::memory_order_relaxed);
    lastProgress   = Clock::now();
    return ma_device_start(&device) == MA_SUCCESS;
}

void AudioDevice::Impl::MarkLost() noexcept
{
    state = DeviceState::Lost;
}

void AudioDevice::Impl::TryReopenOnDefault()
{
    const Clock::time_point now = Clock::now();
    if (now - lastReopenAttempt < kReopenRetryInterval)
    {
        return;
    }
    lastReopenAttempt = now;

    CloseBackendDevice();
    chosen.reset();
    const std::expected<void, AudioError> opened = OpenBackendDevice(std::nullopt);
    if (!opened.has_value())
    {
        Core::Log::Warn("Audio output lost and the default device would not open: {}", ToString(opened.error()));
        return;
    }
    if (!StartBackendDevice())
    {
        Core::Log::Warn("Audio output lost and the default device would not start");
        return;
    }
    state = DeviceState::Running;
    Core::Log::Warn("Audio output lost; now playing on {}", currentName);
}

AudioDevice::AudioDevice(std::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
AudioDevice::AudioDevice(AudioDevice &&) noexcept            = default;
AudioDevice &AudioDevice::operator=(AudioDevice &&) noexcept = default;
AudioDevice::~AudioDevice()                                  = default;

std::expected<AudioDevice, AudioError> AudioDevice::Open(const AudioContext &context, std::optional<DeviceId> chosen)
{
    std::unique_ptr<Impl> impl = std::make_unique<Impl>();
    impl->context              = context.Backend();
    impl->chosen               = chosen;
    const std::expected<void, AudioError> opened = impl->OpenBackendDevice(chosen);
    if (!opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    return AudioDevice(std::move(impl));
}

std::expected<void, AudioError> AudioDevice::Start(AudioRenderer &renderer)
{
    if (_impl->state == DeviceState::Running)
    {
        return std::unexpected(AudioError::AlreadyStarted);
    }
    if (_impl->state == DeviceState::Lost)
    {
        // Nothing is playing; the reopen in Update resumes this renderer.
        _impl->renderer = &renderer;
        return {};
    }

    _impl->renderer = &renderer;
    if (!_impl->StartBackendDevice())
    {
        _impl->MarkLost();
        return std::unexpected(AudioError::DeviceStartFailed);
    }
    _impl->state = DeviceState::Running;
    return {};
}

void AudioDevice::Stop() noexcept
{
    if (_impl->state == DeviceState::Stopped)
    {
        return;
    }
    if (_impl->deviceInitialised)
    {
        ma_device_stop(&_impl->device);
    }
    _impl->renderer = nullptr;
    _impl->state    = DeviceState::Stopped;
}

void AudioDevice::Update()
{
    if (_impl->state == DeviceState::Running)
    {
        const Clock::time_point now = Clock::now();
        const std::uint64_t frames  = _impl->framesRequested.load(std::memory_order_relaxed);
        if (frames != _impl->lastSeenFrames)
        {
            _impl->lastSeenFrames = frames;
            _impl->lastProgress   = now;
        }

        const bool backendStopped = ma_device_get_state(&_impl->device) != ma_device_state_started;
        const bool stalled        = now - _impl->lastProgress > kCallbackStallTimeout;
        if (backendStopped || stalled)
        {
            _impl->MarkLost();
        }
    }

    if (_impl->state == DeviceState::Lost)
    {
        _impl->TryReopenOnDefault();
    }
}

DeviceState AudioDevice::State() const noexcept
{
    return _impl->state;
}

std::optional<DeviceId> AudioDevice::ChosenDevice() const noexcept
{
    return _impl->chosen;
}

std::string_view AudioDevice::CurrentDeviceName() const noexcept
{
    return _impl->currentName;
}

void AudioDeviceTestAccess::MarkLost(AudioDevice &device)
{
    device._impl->MarkLost();
}

std::uint64_t AudioDeviceTestAccess::FramesRequested(const AudioDevice &device)
{
    return device._impl->framesRequested.load(std::memory_order_relaxed);
}

} // namespace Assisi::Audio
