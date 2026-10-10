/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Client.hpp
/// @brief The application's side of Steam: starting it, pumping it each frame,
///        and the stand-in every process gets when Steam is not running.

#include <Assisi/Steam/Steam.hpp>

#include <expected>
#include <memory>

namespace Assisi::Steam
{

/// @brief Steam as the application holds it: Services, plus the per-frame pump.
///        Destroying a connected client shuts Steam down.
class Client : public Services
{
  public:
    /// @brief Handle what Steam reported since the last frame, such as the
    ///        overlay opening. Called once per frame on the main thread.
    virtual void Update() = 0;
};

/// @brief Steam that is not there. Every query reports unavailable, every call
///        changes nothing, and nothing is remembered: this stands in for Steam,
///        it does not imitate it.
class Unavailable final : public Client
{
  public:
    explicit Unavailable(SteamError reason) : _reason(reason) {}

    [[nodiscard]] bool IsAvailable() const override { return false; }
    [[nodiscard]] SteamError Why() const override { return _reason; }
    [[nodiscard]] std::expected<std::string, SteamError> PersonaName() const override;
    [[nodiscard]] std::expected<SteamId, SteamError> UserId() const override;
    [[nodiscard]] bool IsOverlayActive() const override { return false; }
    [[nodiscard]] std::expected<void, SteamError> UnlockAchievement(std::string_view name) override;
    [[nodiscard]] std::expected<bool, SteamError> IsAchievementUnlocked(std::string_view name) const override;
    [[nodiscard]] std::expected<void, SteamError> SetStat(std::string_view name, std::int32_t value) override;
    [[nodiscard]] std::expected<void, SteamError> SetStat(std::string_view name, float value) override;
    [[nodiscard]] std::expected<std::int32_t, SteamError> StatInt(std::string_view name) const override;
    [[nodiscard]] std::expected<float, SteamError> StatFloat(std::string_view name) const override;
    [[nodiscard]] std::expected<void, SteamError> StoreStats() override;
    [[nodiscard]] std::expected<void, SteamError> SetRichPresence(std::string_view key,
                                                                  std::string_view value) override;
    [[nodiscard]] Hardware RunningOn() const override { return Hardware::None; }
    [[nodiscard]] DefaultConfig SuggestedConfig() const override { return DefaultConfig::None; }
    [[nodiscard]] bool IsRunningUnderProton() const override { return false; }
    void Update() override {}

  private:
    SteamError _reason;
};

/// @brief Whether this build was made with the Steamworks SDK.
[[nodiscard]] bool IsBuiltWithSteamworks() noexcept;

/// @brief Start Steam for @p app. At most one client exists per process; Steam
///        allows one. In a build without the SDK this is always NotBuilt.
[[nodiscard]] std::expected<std::unique_ptr<Client>, SteamError> Connect(AppId app);

/// @brief When the game was started outside Steam, ask Steam to start it again
///        through itself, and return true: the caller should then exit at once.
///        False when it was started by Steam, when a steam_appid.txt beside it
///        says it is a development run, or in a build without the SDK.
[[nodiscard]] bool RelaunchThroughSteamIfNeeded(AppId app);

} // namespace Assisi::Steam
