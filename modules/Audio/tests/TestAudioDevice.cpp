/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "AudioDeviceTestAccess.hpp"
#include "CountingRenderer.hpp"
#include "WavBuilder.hpp"

#include <Assisi/Audio/AudioContext.hpp>
#include <Assisi/Audio/AudioDevice.hpp>
#include <Assisi/Audio/AudioFormat.hpp>
#include <Assisi/Audio/Clip.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace Assisi::Audio;
using Testing::CountingRenderer;
using Testing::kPollInterval;
using Testing::WaitForFrames;

namespace
{

/// @brief A tenth of a second: long enough to span several device periods.
constexpr std::uint64_t kShortClipFrames = kSampleRate / 10;

/// @brief A clip whose samples are distinct, so a renderer copying from the
/// wrong offset would be visible to anyone listening for it.
PcmClip MakeRampClip()
{
    PcmClip clip;
    clip.samples.resize(static_cast<std::size_t>(kShortClipFrames) * kChannelCount);
    for (std::size_t i = 0; i < clip.samples.size(); ++i)
    {
        clip.samples[i] = static_cast<float>(i % kSampleRate) / static_cast<float>(kSampleRate);
    }
    return clip;
}

AudioContext NullContext()
{
    std::expected<AudioContext, AudioError> context = AudioContext::Create(AudioBackend::Null);
    REQUIRE(context.has_value());
    return std::move(*context);
}

AudioDevice OpenOn(const AudioContext &context, std::optional<DeviceId> chosen)
{
    std::expected<AudioDevice, AudioError> device = AudioDevice::Open(context, chosen);
    REQUIRE(device.has_value());
    return std::move(*device);
}

} // namespace

TEST_CASE("A started device plays a whole clip through its renderer on the audio thread")
{
    const PcmClip clip = MakeRampClip();
    CountingRenderer renderer(clip);
    const AudioContext context = NullContext();
    AudioDevice device         = OpenOn(context, std::nullopt);
    CHECK(device.State() == DeviceState::Stopped);

    REQUIRE(device.Start(renderer).has_value());
    CHECK(device.State() == DeviceState::Running);

    REQUIRE(WaitForFrames(renderer, clip.Frames()));
    device.Stop();
    CHECK(device.State() == DeviceState::Stopped);

    CHECK(renderer.ClipCursor() == clip.Frames());
    CHECK(renderer.PartialFrameSpans() == 0);
    CHECK(renderer.FramesRendered() == AudioDeviceTestAccess::FramesRequested(device));
    CHECK(renderer.RenderThread() != std::this_thread::get_id());

    const std::uint64_t framesAtStop = renderer.FramesRendered();
    std::this_thread::sleep_for(kPollInterval * 4);
    CHECK(renderer.FramesRendered() == framesAtStop);
}

TEST_CASE("Starting a running device is refused")
{
    const PcmClip clip = MakeRampClip();
    CountingRenderer renderer(clip);
    const AudioContext context = NullContext();
    AudioDevice device         = OpenOn(context, std::nullopt);

    REQUIRE(device.Start(renderer).has_value());
    const std::expected<void, AudioError> second = device.Start(renderer);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == AudioError::AlreadyStarted);
}

TEST_CASE("A device opened on a listed device remembers which one it was asked for")
{
    const AudioContext context = NullContext();
    const std::expected<std::vector<DeviceInfo>, AudioError> outputs = context.ListDevices(DeviceKind::Output);
    REQUIRE(outputs.has_value());
    REQUIRE_FALSE(outputs->empty());
    const DeviceId chosen = outputs->front().id;

    const PcmClip clip = MakeRampClip();
    CountingRenderer renderer(clip);
    AudioDevice device = OpenOn(context, chosen);
    CHECK(device.ChosenDevice() == chosen);
    CHECK(device.CurrentDeviceName() == outputs->front().name);

    REQUIRE(device.Start(renderer).has_value());
    CHECK(WaitForFrames(renderer, clip.Frames()));
}

TEST_CASE("Opening a device that is no longer listed opens the default device instead")
{
    const AudioContext context = NullContext();
    DeviceId gone;
    gone.bytes.fill(std::byte{0x5A});

    const PcmClip clip = MakeRampClip();
    CountingRenderer renderer(clip);
    AudioDevice device = OpenOn(context, gone);
    CHECK_FALSE(device.ChosenDevice().has_value());

    REQUIRE(device.Start(renderer).has_value());
    CHECK(WaitForFrames(renderer, 1));
}

TEST_CASE("Update leaves a healthy device alone")
{
    const AudioContext context = NullContext();
    const std::expected<std::vector<DeviceInfo>, AudioError> outputs = context.ListDevices(DeviceKind::Output);
    REQUIRE(outputs.has_value());
    const DeviceId chosen = outputs->front().id;

    const PcmClip clip = MakeRampClip();
    CountingRenderer renderer(clip);
    AudioDevice device = OpenOn(context, chosen);
    REQUIRE(device.Start(renderer).has_value());
    REQUIRE(WaitForFrames(renderer, 1));

    device.Update();

    CHECK(device.State() == DeviceState::Running);
    CHECK(device.ChosenDevice() == chosen);
    const std::uint64_t framesAfterUpdate = renderer.FramesRendered();
    CHECK(WaitForFrames(renderer, framesAfterUpdate + 1));
}

TEST_CASE("Update does not mistake a requested stop for a lost device")
{
    const AudioContext context = NullContext();
    const std::expected<std::vector<DeviceInfo>, AudioError> outputs = context.ListDevices(DeviceKind::Output);
    REQUIRE(outputs.has_value());
    const DeviceId chosen = outputs->front().id;

    const PcmClip clip = MakeRampClip();
    CountingRenderer renderer(clip);
    AudioDevice device = OpenOn(context, chosen);
    REQUIRE(device.Start(renderer).has_value());
    REQUIRE(WaitForFrames(renderer, 1));
    device.Stop();

    device.Update();

    CHECK(device.State() == DeviceState::Stopped);
    CHECK(device.ChosenDevice() == chosen);
}

TEST_CASE("Update moves a lost device to the default device and resumes the same renderer")
{
    const AudioContext context = NullContext();
    const std::expected<std::vector<DeviceInfo>, AudioError> outputs = context.ListDevices(DeviceKind::Output);
    REQUIRE(outputs.has_value());

    const PcmClip clip = MakeRampClip();
    CountingRenderer renderer(clip);
    AudioDevice device = OpenOn(context, outputs->front().id);
    REQUIRE(device.Start(renderer).has_value());
    REQUIRE(WaitForFrames(renderer, 1));

    AudioDeviceTestAccess::MarkLost(device);
    CHECK(device.State() == DeviceState::Lost);
    const std::uint64_t framesAtLoss = renderer.FramesRendered();

    device.Update();

    CHECK(device.State() == DeviceState::Running);
    CHECK_FALSE(device.ChosenDevice().has_value());
    CHECK(WaitForFrames(renderer, framesAtLoss + 1));
}

namespace
{

/// @brief Loud enough to hear clearly, well short of clipping.
constexpr float kBeepAmplitude = 0.25f;
constexpr float kBeepFrequencyHz = 440.0f;
constexpr std::uint64_t kBeepFrames = kSampleRate / 2;
/// @brief Long enough to unplug a headset by hand while it plays.
constexpr std::uint64_t kUnplugBeepFrames = kSampleRate * 10;

constexpr float kPcm16Max = 32767.0f;

/// @brief A sine beep as a mono 16-bit WAV, decoded the way a shipped sound will be.
PcmClip DecodedBeep(std::uint64_t frames)
{
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames));
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const float phase = 2.0f * std::numbers::pi_v<float> *kBeepFrequencyHz * static_cast<float>(i) /
                            static_cast<float>(kSampleRate);
        samples[i] = static_cast<std::int16_t>(kBeepAmplitude * kPcm16Max * std::sin(phase));
    }
    const std::expected<PcmClip, AudioError> clip = DecodeClip(Testing::BuildWav(samples, kSampleRate, 1));
    REQUIRE(clip.has_value());
    return *clip;
}

/// @brief A device's name as a log line shows it, marked when it is the default.
std::string Described(const DeviceInfo &device)
{
    return device.isDefault ? device.name + " (default)" : device.name;
}

void PlayToEnd(AudioDevice &device, CountingRenderer &renderer, std::uint64_t frames)
{
    REQUIRE(device.Start(renderer).has_value());
    while (renderer.ClipCursor() < frames && device.State() != DeviceState::Stopped)
    {
        device.Update();
        std::this_thread::sleep_for(kPollInterval);
    }
    device.Stop();
}

} // namespace

TEST_CASE("Manual: a decoded WAV beeps once on every output device" * doctest::skip())
{
    std::expected<AudioContext, AudioError> context = AudioContext::Create(AudioBackend::Default);
    REQUIRE(context.has_value());
    MESSAGE("Backend: " << context->BackendName());

    const std::expected<std::vector<DeviceInfo>, AudioError> outputs = context->ListDevices(DeviceKind::Output);
    REQUIRE(outputs.has_value());
    const std::expected<std::vector<DeviceInfo>, AudioError> inputs = context->ListDevices(DeviceKind::Input);
    REQUIRE(inputs.has_value());
    for (const DeviceInfo &input : *inputs)
    {
        MESSAGE("Input: " << Described(input));
    }

    const PcmClip beep = DecodedBeep(kBeepFrames);
    for (const DeviceInfo &output : *outputs)
    {
        MESSAGE("Beeping on: " << Described(output));
        CountingRenderer renderer(beep);
        AudioDevice device = OpenOn(*context, output.id);
        PlayToEnd(device, renderer, beep.Frames());
    }
}

TEST_CASE("Manual: unplug the device mid-beep and the beep carries on elsewhere" * doctest::skip())
{
    std::expected<AudioContext, AudioError> context = AudioContext::Create(AudioBackend::Default);
    REQUIRE(context.has_value());

    const PcmClip beep = DecodedBeep(kUnplugBeepFrames);
    for (const bool explicitDevice : {false, true})
    {
        std::optional<DeviceId> chosen;
        if (explicitDevice)
        {
            // Listed again, since the first run may have unplugged the default.
            const std::expected<std::vector<DeviceInfo>, AudioError> outputs =
                context->ListDevices(DeviceKind::Output);
            REQUIRE(outputs.has_value());
            for (const DeviceInfo &output : *outputs)
            {
                if (output.isDefault)
                {
                    chosen = output.id;
                }
            }
        }
        CountingRenderer renderer(beep);
        AudioDevice device = OpenOn(*context, chosen);
        const std::string how = explicitDevice ? " by id" : " as the default";
        MESSAGE("Playing on " << device.CurrentDeviceName() << how << "; unplug it now.");
        PlayToEnd(device, renderer, beep.Frames());
        MESSAGE("Finished on " << device.CurrentDeviceName());
    }
}
