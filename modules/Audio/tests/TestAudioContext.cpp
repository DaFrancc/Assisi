/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/AudioContext.hpp>
#include <Assisi/Audio/AudioDeviceInfo.hpp>

#include <doctest/doctest.h>

#include <expected>
#include <vector>

using namespace Assisi::Audio;

TEST_CASE("The null backend lists its one output and one input device")
{
    std::expected<AudioContext, AudioError> context = AudioContext::Create(AudioBackend::Null);
    REQUIRE(context.has_value());

    const std::expected<std::vector<DeviceInfo>, AudioError> outputs = context->ListDevices(DeviceKind::Output);
    REQUIRE(outputs.has_value());
    REQUIRE(outputs->size() == 1);
    CHECK((*outputs)[0].name == "NULL Playback Device");
    CHECK((*outputs)[0].isDefault);

    const std::expected<std::vector<DeviceInfo>, AudioError> inputs = context->ListDevices(DeviceKind::Input);
    REQUIRE(inputs.has_value());
    REQUIRE(inputs->size() == 1);
    CHECK((*inputs)[0].name == "NULL Capture Device");
    CHECK((*inputs)[0].isDefault);
}
