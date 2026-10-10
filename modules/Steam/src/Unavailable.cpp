/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Steam/Client.hpp>

#include <array>
#include <cstddef>

namespace Assisi::Steam
{

std::string_view ToString(SteamError error) noexcept
{
    static constexpr std::array<std::string_view, static_cast<std::size_t>(SteamError::Count_)> kNames = {
        "this process does not use Steam",
        "the engine was built without the Steamworks SDK",
        "the Steam client is not running, or not as this user",
        "the Steam client is older than the game's Steamworks SDK; update Steam",
        "Steam would not start for this game: check its app id, and that this account owns it",
        "Steam is unavailable",
        "Steam refused the call: is that name defined for this app on the Steamworks site?",
    };
    const std::size_t index = static_cast<std::size_t>(error);
    return index < kNames.size() ? kNames[index] : std::string_view{"unknown Steam error"};
}

std::expected<std::string, SteamError> Unavailable::PersonaName() const
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<SteamId, SteamError> Unavailable::UserId() const
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<void, SteamError> Unavailable::UnlockAchievement(std::string_view /*name*/)
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<bool, SteamError> Unavailable::IsAchievementUnlocked(std::string_view /*name*/) const
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<void, SteamError> Unavailable::SetStat(std::string_view /*name*/, std::int32_t /*value*/)
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<void, SteamError> Unavailable::SetStat(std::string_view /*name*/, float /*value*/)
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<std::int32_t, SteamError> Unavailable::StatInt(std::string_view /*name*/) const
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<float, SteamError> Unavailable::StatFloat(std::string_view /*name*/) const
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<void, SteamError> Unavailable::StoreStats()
{
    return std::unexpected(SteamError::Unavailable);
}

std::expected<void, SteamError> Unavailable::SetRichPresence(std::string_view /*key*/, std::string_view /*value*/)
{
    return std::unexpected(SteamError::Unavailable);
}

} // namespace Assisi::Steam
