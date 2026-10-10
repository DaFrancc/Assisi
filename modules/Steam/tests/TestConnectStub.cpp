/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
// Compiled only into a build without the Steamworks SDK.
#include <doctest/doctest.h>

#include <Assisi/Steam/Client.hpp>

using Assisi::Steam::AppId;
using Assisi::Steam::SteamError;

// The Spacewar test app Valve makes available to every developer.
constexpr AppId kTestApp{480};

TEST_CASE("Without the SDK Steam never starts and the game never relaunches")
{
    CHECK_FALSE(Assisi::Steam::IsBuiltWithSteamworks());
    CHECK(Assisi::Steam::Connect(kTestApp).error() == SteamError::NotBuilt);
    CHECK_FALSE(Assisi::Steam::RelaunchThroughSteamIfNeeded(kTestApp));
}
