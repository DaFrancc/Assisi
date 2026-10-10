/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <doctest/doctest.h>

#include <Assisi/Steam/Client.hpp>

#include <cstddef>
#include <set>
#include <string_view>

using Assisi::Steam::DefaultConfig;
using Assisi::Steam::Hardware;
using Assisi::Steam::SteamError;
using Assisi::Steam::Unavailable;

TEST_CASE("Unavailable Steam answers every question with why it is unavailable")
{
    Unavailable steam(SteamError::NoClient);

    CHECK_FALSE(steam.IsAvailable());
    CHECK(steam.Why() == SteamError::NoClient);
    CHECK(steam.PersonaName().error() == SteamError::Unavailable);
    CHECK(steam.UserId().error() == SteamError::Unavailable);
    CHECK_FALSE(steam.IsOverlayActive());
    CHECK(steam.StatInt("kills").error() == SteamError::Unavailable);
    CHECK(steam.StatFloat("distance").error() == SteamError::Unavailable);
    CHECK(steam.StoreStats().error() == SteamError::Unavailable);
    CHECK(steam.SetRichPresence("status", "Hunting").error() == SteamError::Unavailable);
    CHECK(steam.RunningOn() == Hardware::None);
    CHECK(steam.SuggestedConfig() == DefaultConfig::None);
    CHECK_FALSE(steam.IsRunningUnderProton());
}

// The stand-in must never pass for Steam: Valve's agreement forbids software that
// replaces the SDK's functionality, and a game testing an achievement against a
// stub that quietly kept it would believe something Steam never recorded.
TEST_CASE("Unavailable Steam remembers nothing it was told")
{
    Unavailable steam(SteamError::NotBuilt);

    CHECK(steam.UnlockAchievement("FIRST_HUNT").error() == SteamError::Unavailable);
    CHECK(steam.IsAchievementUnlocked("FIRST_HUNT").error() == SteamError::Unavailable);
    CHECK(steam.SetStat("kills", 3).error() == SteamError::Unavailable);
    CHECK(steam.StatInt("kills").error() == SteamError::Unavailable);
    CHECK(steam.SetStat("distance", 1.5f).error() == SteamError::Unavailable);
    CHECK(steam.StatFloat("distance").error() == SteamError::Unavailable);
}

TEST_CASE("Unavailable Steam can be pumped every frame")
{
    Unavailable steam(SteamError::NotRequested);
    steam.Update();
    CHECK(steam.Why() == SteamError::NotRequested);
}

TEST_CASE("Every Steam error has its own description")
{
    std::set<std::string_view> seen;
    for (std::size_t index = 0; index < static_cast<std::size_t>(SteamError::Count_); ++index)
    {
        const std::string_view text = Assisi::Steam::ToString(static_cast<SteamError>(index));
        CHECK_FALSE(text.empty());
        CHECK(seen.insert(text).second);
    }
}
