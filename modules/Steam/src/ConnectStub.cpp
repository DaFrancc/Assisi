/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file ConnectStub.cpp
/// @brief Starting Steam in a build without the Steamworks SDK: it never starts.
///        CMake compiles this file or Steamworks.cpp, never both.
#include <Assisi/Steam/Client.hpp>

namespace Assisi::Steam
{

bool IsBuiltWithSteamworks() noexcept
{
    return false;
}

std::expected<std::unique_ptr<Client>, SteamError> Connect(AppId /*app*/)
{
    return std::unexpected(SteamError::NotBuilt);
}

bool RelaunchThroughSteamIfNeeded(AppId /*app*/)
{
    return false;
}

} // namespace Assisi::Steam
