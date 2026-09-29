/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/App/OptionsConfig.hpp>
#include <Assisi/App/PlayerSettings.hpp>
#include <Assisi/Audio/BusConfig.hpp>
#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Mixer.hpp>

#include <expected>
#include <memory>
#include <string_view>
#include <vector>

using namespace Assisi;

namespace
{

std::unique_ptr<Audio::Mixer> MakeMixer()
{
    std::expected<std::unique_ptr<Audio::Mixer>, Audio::AudioError> mixer =
        Audio::Mixer::Create(Audio::BusLayout::Defaults());
    REQUIRE(mixer.has_value());
    return std::move(*mixer);
}

} // namespace

TEST_CASE("Applying audio settings sets each bus the options name, and only those")
{
    std::unique_ptr<Audio::Mixer> mixer = MakeMixer();
    App::OptionsConfig options;
    App::PlayerSettings settings(options, mixer.get());

    settings.Options().busVolumes["Music"] = 0.25f;
    CHECK(mixer->BusVolume(Audio::ToBusId(Audio::DefaultBus::Music)) == doctest::Approx(1.0f));

    settings.ApplyAudio();
    CHECK(mixer->BusVolume(Audio::ToBusId(Audio::DefaultBus::Music)) == doctest::Approx(0.25f));
    CHECK(mixer->BusVolume(Audio::ToBusId(Audio::DefaultBus::Sfx)) == doctest::Approx(1.0f));
}

TEST_CASE("A volume for a bus the game does not have is skipped and kept")
{
    std::unique_ptr<Audio::Mixer> mixer = MakeMixer();
    App::OptionsConfig options;
    App::PlayerSettings settings(options, mixer.get());

    settings.Options().busVolumes["Footsteps"] = 0.5f;
    settings.ApplyAudio();
    CHECK(options.busVolumes.contains("Footsteps"));
}

TEST_CASE("Settings list every bus, and show the player's volume where set and the game's elsewhere")
{
    Audio::BusConfig config;
    config.buses.push_back(Audio::BusDeclaration{.name   = Core::InternedString{"Footsteps"},
                                                 .parent = Core::InternedString{"SFX"},
                                                 .volume = 0.75f});
    const std::expected<Audio::BusLayout, Audio::AudioError> layout = Audio::BusLayout::FromConfig(config);
    REQUIRE(layout.has_value());
    std::expected<std::unique_ptr<Audio::Mixer>, Audio::AudioError> mixer = Audio::Mixer::Create(*layout);
    REQUIRE(mixer.has_value());

    App::OptionsConfig options;
    options.busVolumes["Music"] = 0.25f;
    const App::PlayerSettings settings(options, mixer->get());

    const std::vector<std::string_view> buses = settings.Buses();
    REQUIRE(buses.size() == layout->Count());
    CHECK(buses.front() == "Master");
    CHECK(buses.back() == "Footsteps");

    CHECK(settings.BusVolume("Music") == doctest::Approx(0.25f));
    CHECK(settings.BusVolume("Footsteps") == doctest::Approx(0.75f));
    CHECK(settings.BusVolume("SFX") == doctest::Approx(1.0f));
    CHECK_FALSE(settings.BusVolume("Nowhere").has_value());
}

TEST_CASE("Settings without a mixer change the options and apply nothing")
{
    App::OptionsConfig options;
    App::PlayerSettings settings(options, nullptr);

    settings.Options().busVolumes["Music"] = 0.5f;
    settings.ApplyAudio();
    CHECK(options.busVolumes.at("Music") == doctest::Approx(0.5f));
}
