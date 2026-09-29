/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/BusConfig.hpp>
#include <Assisi/Audio/BusLayout.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <utility>

using namespace Assisi::Audio;
using Assisi::Core::InternedString;

namespace
{

BusDeclaration Declare(std::string_view name, std::string_view parent)
{
    return BusDeclaration{.name = InternedString{name}, .parent = InternedString{parent}, .volume = 1.0f};
}

} // namespace

TEST_CASE("The default layout is Master with every other default bus under it")
{
    const BusLayout layout = BusLayout::Defaults();
    REQUIRE(layout.Count() == std::to_underlying(DefaultBus::Count));
    CHECK_FALSE(layout.Parent(ToBusId(DefaultBus::Master)).has_value());
    CHECK(layout.FindBus("Master") == ToBusId(DefaultBus::Master));
    CHECK(layout.FindBus("SFX") == ToBusId(DefaultBus::Sfx));

    for (std::size_t i = 1; i < layout.Count(); ++i)
    {
        const BusId bus{static_cast<std::uint16_t>(i)};
        CHECK(layout.Parent(bus) == ToBusId(DefaultBus::Master));
    }
}

TEST_CASE("A declared bus joins the tree under the parent it names")
{
    BusConfig config;
    config.buses.push_back(Declare("Footsteps", "SFX"));
    config.buses.push_back(Declare("Gravel", "Footsteps"));

    const std::expected<BusLayout, AudioError> layout = BusLayout::FromConfig(config);
    REQUIRE(layout.has_value());

    const std::optional<BusId> footsteps = layout->FindBus("Footsteps");
    const std::optional<BusId> gravel    = layout->FindBus("Gravel");
    REQUIRE(footsteps.has_value());
    REQUIRE(gravel.has_value());
    CHECK(layout->Parent(*footsteps) == ToBusId(DefaultBus::Sfx));
    CHECK(layout->Parent(*gravel) == footsteps);
    CHECK(layout->Name(*gravel) == "Gravel");
}

TEST_CASE("A bus whose parent is declared after it is refused")
{
    BusConfig config;
    config.buses.push_back(Declare("Gravel", "Footsteps"));
    config.buses.push_back(Declare("Footsteps", "SFX"));

    const std::expected<BusLayout, AudioError> layout = BusLayout::FromConfig(config);
    REQUIRE_FALSE(layout.has_value());
    CHECK(layout.error() == AudioError::UnknownParentBus);
}

TEST_CASE("A bus named twice, or named like a default bus, is refused")
{
    BusConfig twice;
    twice.buses.push_back(Declare("Footsteps", "SFX"));
    twice.buses.push_back(Declare("Footsteps", "Master"));
    const std::expected<BusLayout, AudioError> fromTwice = BusLayout::FromConfig(twice);
    REQUIRE_FALSE(fromTwice.has_value());
    CHECK(fromTwice.error() == AudioError::DuplicateBus);

    BusConfig redeclared;
    redeclared.buses.push_back(Declare("Music", "Master"));
    const std::expected<BusLayout, AudioError> fromRedeclared = BusLayout::FromConfig(redeclared);
    REQUIRE_FALSE(fromRedeclared.has_value());
    CHECK(fromRedeclared.error() == AudioError::DuplicateBus);
}

TEST_CASE("More buses than the mixer holds are refused")
{
    BusConfig config;
    for (std::size_t i = 0; i < kMaxBuses; ++i)
    {
        config.buses.push_back(Declare("Bus" + std::to_string(i), "Master"));
    }

    const std::expected<BusLayout, AudioError> layout = BusLayout::FromConfig(config);
    REQUIRE_FALSE(layout.has_value());
    CHECK(layout.error() == AudioError::TooManyBuses);
}
