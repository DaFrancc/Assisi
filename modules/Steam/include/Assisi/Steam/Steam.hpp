/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Steam.hpp
/// @brief What game systems ask Steam for: the player, achievements, stats,
///        rich presence, the overlay and the hardware the game runs on.
///
/// Every call works whether or not Steam is there. Without it — a build
/// without the Steamworks SDK, the editor, a headless run, or a client that is
/// not running — each answers that Steam is unavailable and changes nothing, so
/// game code calls these unconditionally and never checks first.
///
/// Only the engine's own types appear here. The SDK's headers are included by
/// one source file in this module, so nothing that includes this header needs
/// them, and the engine builds without them.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace Assisi::Steam
{

/// @brief A game's Steam application id, which Valve assigns.
struct AppId
{
    std::uint32_t value = 0;
};

/// @brief A Steam account's 64-bit id.
struct SteamId
{
    std::uint64_t value = 0;
};

/// @brief Why Steam cannot do something.
enum class SteamError : std::uint8_t
{
    NotRequested,    ///< This process does not use Steam: the editor, a headless run, or a game with no app id.
    NotBuilt,        ///< The engine was built without the Steamworks SDK.
    NoClient,        ///< The Steam client is not running, or not as this user.
    VersionMismatch, ///< The Steam client is older than the SDK the game was built with.
    Failed,          ///< Steam would not start for another reason: no app id it accepts, or no license for it.
    Unavailable,     ///< Asked of Steam while it is unavailable, for the reason Why() gives.
    Rejected,        ///< Steam refused the call: an achievement or stat the app does not define, say.
    Count_,
};

/// @brief A short description, for a log line.
[[nodiscard]] std::string_view ToString(SteamError error) noexcept;

/// @brief The Steam hardware the game runs on, for analytics and support.
enum class Hardware : std::uint8_t
{
    None,
    SteamDeck,
    SteamMachine,
    SteamFrame,
    Unknown, ///< Hardware newer than this engine knows about.
    Count_,
};

/// @brief The settings preset Steam suggests for this hardware.
enum class DefaultConfig : std::uint8_t
{
    None,
    Low,
    Medium,
    High,
    Max,
    SteamDeck,
    SteamMachine,
    SteamFrame,
    Unknown, ///< A preset newer than this engine knows about.
    Count_,
};

/// @brief Steam, as game systems reach it through SystemContext::steam.
class Services
{
  public:
    virtual ~Services() = default;

    /// @brief Whether Steam is running for this game. When false, Why() says why
    ///        and every other call reports SteamError::Unavailable.
    [[nodiscard]] virtual bool IsAvailable() const = 0;
    [[nodiscard]] virtual SteamError Why() const = 0;

    [[nodiscard]] virtual std::expected<std::string, SteamError> PersonaName() const = 0;
    [[nodiscard]] virtual std::expected<SteamId, SteamError> UserId() const = 0;

    /// @brief Whether the Steam overlay is open, which a game usually pauses for.
    [[nodiscard]] virtual bool IsOverlayActive() const = 0;

    /// @brief Unlock an achievement by the API name set on the Steamworks site.
    ///        Steam shows it to the player once StoreStats sends it.
    [[nodiscard]] virtual std::expected<void, SteamError> UnlockAchievement(std::string_view name) = 0;
    [[nodiscard]] virtual std::expected<bool, SteamError> IsAchievementUnlocked(std::string_view name) const = 0;

    [[nodiscard]] virtual std::expected<void, SteamError> SetStat(std::string_view name, std::int32_t value) = 0;
    [[nodiscard]] virtual std::expected<void, SteamError> SetStat(std::string_view name, float value) = 0;
    [[nodiscard]] virtual std::expected<std::int32_t, SteamError> StatInt(std::string_view name) const = 0;
    [[nodiscard]] virtual std::expected<float, SteamError> StatFloat(std::string_view name) const = 0;

    /// @brief Send changed achievements and stats to Steam.
    [[nodiscard]] virtual std::expected<void, SteamError> StoreStats() = 0;

    /// @brief Set what friends see the player doing; an empty value clears the key.
    [[nodiscard]] virtual std::expected<void, SteamError> SetRichPresence(std::string_view key,
                                                                          std::string_view value) = 0;

    [[nodiscard]] virtual Hardware RunningOn() const = 0;
    [[nodiscard]] virtual DefaultConfig SuggestedConfig() const = 0;
    [[nodiscard]] virtual bool IsRunningUnderProton() const = 0;
};

} // namespace Assisi::Steam
