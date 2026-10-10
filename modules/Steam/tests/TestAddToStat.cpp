/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <doctest/doctest.h>

#include <Assisi/Steam/Client.hpp>

#include <map>
#include <string>

using Assisi::Steam::DefaultConfig;
using Assisi::Steam::Hardware;
using Assisi::Steam::Services;
using Assisi::Steam::SteamError;
using Assisi::Steam::SteamId;

namespace
{

/// Stats in a map, so AddToStat is tested against values it can see rather
/// than against Steam. Only the stats it knows by name exist.
class RecordingStats final : public Services
{
public:
    std::map<std::string, std::int32_t> ints;
    std::map<std::string, float> floats;
    std::int32_t writes = 0;

    [[nodiscard]] bool IsAvailable() const override { return true; }
    [[nodiscard]] SteamError Why() const override { return SteamError::Unavailable; }
    [[nodiscard]] std::expected<std::string, SteamError> PersonaName() const override { return std::string(); }
    [[nodiscard]] std::expected<SteamId, SteamError> UserId() const override { return SteamId{}; }
    [[nodiscard]] bool IsOverlayActive() const override { return false; }
    [[nodiscard]] std::expected<void, SteamError> UnlockAchievement(std::string_view /*name*/) override { return {}; }
    [[nodiscard]] std::expected<bool, SteamError> IsAchievementUnlocked(std::string_view /*name*/) const override
    {
        return false;
    }

    [[nodiscard]] std::expected<void, SteamError> SetStat(std::string_view name, std::int32_t value) override
    {
        const std::map<std::string, std::int32_t>::iterator found = ints.find(std::string(name));
        if (found == ints.end())
        {
            return std::unexpected(SteamError::Rejected);
        }
        found->second = value;
        ++writes;
        return {};
    }

    [[nodiscard]] std::expected<void, SteamError> SetStat(std::string_view name, float value) override
    {
        const std::map<std::string, float>::iterator found = floats.find(std::string(name));
        if (found == floats.end())
        {
            return std::unexpected(SteamError::Rejected);
        }
        found->second = value;
        ++writes;
        return {};
    }

    [[nodiscard]] std::expected<std::int32_t, SteamError> StatInt(std::string_view name) const override
    {
        const std::map<std::string, std::int32_t>::const_iterator found = ints.find(std::string(name));
        if (found == ints.end())
        {
            return std::unexpected(SteamError::Rejected);
        }
        return found->second;
    }

    [[nodiscard]] std::expected<float, SteamError> StatFloat(std::string_view name) const override
    {
        const std::map<std::string, float>::const_iterator found = floats.find(std::string(name));
        if (found == floats.end())
        {
            return std::unexpected(SteamError::Rejected);
        }
        return found->second;
    }

    [[nodiscard]] std::expected<void, SteamError> StoreStats() override { return {}; }
    [[nodiscard]] std::expected<void, SteamError> SetRichPresence(std::string_view /*key*/,
                                                                  std::string_view /*value*/) override
    {
        return {};
    }
    [[nodiscard]] Hardware RunningOn() const override { return Hardware::None; }
    [[nodiscard]] DefaultConfig SuggestedConfig() const override { return DefaultConfig::None; }
    [[nodiscard]] bool IsRunningUnderProton() const override { return false; }
};

} // namespace

TEST_CASE("AddToStat adds to the stat and returns the new value")
{
    RecordingStats steam;
    steam.ints["kills"] = 4;
    steam.floats["distance"] = 1.5f;

    CHECK(steam.AddToStat("kills", 3).value() == 7);
    CHECK(steam.ints["kills"] == 7);
    CHECK(steam.AddToStat("kills", -2).value() == 5);
    CHECK(steam.AddToStat("distance", 2.25f).value() == doctest::Approx(3.75f));
    CHECK(steam.floats["distance"] == doctest::Approx(3.75f));
}

TEST_CASE("AddToStat on a stat Steam does not know changes nothing and says so")
{
    RecordingStats steam;
    CHECK(steam.AddToStat("unknown", 1).error() == SteamError::Rejected);
    CHECK(steam.AddToStat("unknown", 1.f).error() == SteamError::Rejected);
    CHECK(steam.writes == 0);
}

TEST_CASE("AddToStat on unavailable Steam reports it unavailable")
{
    Assisi::Steam::Unavailable steam(SteamError::NoClient);
    CHECK(steam.AddToStat("kills", 1).error() == SteamError::Unavailable);
    CHECK(steam.AddToStat("distance", 1.f).error() == SteamError::Unavailable);
}
