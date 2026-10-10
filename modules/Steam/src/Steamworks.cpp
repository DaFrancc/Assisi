/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Steamworks.cpp
/// @brief Steam through the Steamworks SDK. The only file in the engine that
///        includes the SDK's headers, which come from the developer's own local
///        copy; CMake compiles this file or ConnectStub.cpp, never both.
///
/// Callbacks are dispatched by hand rather than through the SDK's callback
/// objects: Update reads every message Steam queued since the last frame, on
/// the main thread, so nothing Steam reports arrives anywhere else or at any
/// other time.
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Steam/Client.hpp>

#include <steam/steam_api.h>

#include <string>

namespace Assisi::Steam
{
namespace
{

/// The SDK's C strings need a terminator a string_view does not promise.
std::string Terminated(std::string_view text)
{
    return std::string(text);
}

std::expected<void, SteamError> Checked(bool accepted)
{
    if (!accepted)
    {
        return std::unexpected(SteamError::Rejected);
    }
    return {};
}

Hardware ToHardware(ESteamHardwareType type)
{
    switch (type)
    {
    case k_ESteamHardwareTypeNone:
        return Hardware::None;
    case k_ESteamHardwareTypeSteamDeck:
        return Hardware::SteamDeck;
    case k_ESteamHardwareTypeSteamMachine:
        return Hardware::SteamMachine;
    case k_ESteamHardwareTypeSteamFrame:
        return Hardware::SteamFrame;
    }
    return Hardware::Unknown;
}

DefaultConfig ToDefaultConfig(ESteamHardwareDefaultConfig config)
{
    switch (config)
    {
    case k_ESteamHardwareDefaultConfigNone:
        return DefaultConfig::None;
    case k_ESteamHardwareDefaultConfigLow:
        return DefaultConfig::Low;
    case k_ESteamHardwareDefaultConfigMedium:
        return DefaultConfig::Medium;
    case k_ESteamHardwareDefaultConfigHigh:
        return DefaultConfig::High;
    case k_ESteamHardwareDefaultConfigMax:
        return DefaultConfig::Max;
    case k_ESteamHardwareDefaultConfigSteamDeck:
        return DefaultConfig::SteamDeck;
    case k_ESteamHardwareDefaultConfigSteamMachine:
        return DefaultConfig::SteamMachine;
    case k_ESteamHardwareDefaultConfigSteamFrame:
        return DefaultConfig::SteamFrame;
    }
    return DefaultConfig::Unknown;
}

SteamError ToError(ESteamAPIInitResult result)
{
    switch (result)
    {
    case k_ESteamAPIInitResult_OK:
        break;
    case k_ESteamAPIInitResult_NoSteamClient:
        return SteamError::NoClient;
    case k_ESteamAPIInitResult_VersionMismatch:
        return SteamError::VersionMismatch;
    case k_ESteamAPIInitResult_FailedGeneric:
        return SteamError::Failed;
    }
    return SteamError::Failed;
}

class Steamworks final : public Client
{
  public:
    Steamworks() = default;
    ~Steamworks() override { SteamAPI_Shutdown(); }

    Steamworks(const Steamworks &) = delete;
    Steamworks &operator=(const Steamworks &) = delete;

    [[nodiscard]] bool IsAvailable() const override { return true; }
    [[nodiscard]] SteamError Why() const override { return SteamError::Unavailable; }

    [[nodiscard]] std::expected<std::string, SteamError> PersonaName() const override
    {
        // Copied at once: the SDK's pointer is only good until its next call.
        return std::string(SteamFriends()->GetPersonaName());
    }

    [[nodiscard]] std::expected<SteamId, SteamError> UserId() const override
    {
        return SteamId{SteamUser()->GetSteamID().ConvertToUint64()};
    }

    [[nodiscard]] bool IsOverlayActive() const override { return _overlayActive; }

    [[nodiscard]] std::expected<void, SteamError> UnlockAchievement(std::string_view name) override
    {
        return Checked(SteamUserStats()->SetAchievement(Terminated(name).c_str()));
    }

    [[nodiscard]] std::expected<bool, SteamError> IsAchievementUnlocked(std::string_view name) const override
    {
        bool unlocked = false;
        if (!SteamUserStats()->GetAchievement(Terminated(name).c_str(), &unlocked))
        {
            return std::unexpected(SteamError::Rejected);
        }
        return unlocked;
    }

    [[nodiscard]] std::expected<void, SteamError> SetStat(std::string_view name, std::int32_t value) override
    {
        return Checked(SteamUserStats()->SetStat(Terminated(name).c_str(), value));
    }

    [[nodiscard]] std::expected<void, SteamError> SetStat(std::string_view name, float value) override
    {
        return Checked(SteamUserStats()->SetStat(Terminated(name).c_str(), value));
    }

    [[nodiscard]] std::expected<std::int32_t, SteamError> StatInt(std::string_view name) const override
    {
        int32 value = 0;
        if (!SteamUserStats()->GetStat(Terminated(name).c_str(), &value))
        {
            return std::unexpected(SteamError::Rejected);
        }
        return static_cast<std::int32_t>(value);
    }

    [[nodiscard]] std::expected<float, SteamError> StatFloat(std::string_view name) const override
    {
        float value = 0.f;
        if (!SteamUserStats()->GetStat(Terminated(name).c_str(), &value))
        {
            return std::unexpected(SteamError::Rejected);
        }
        return value;
    }

    [[nodiscard]] std::expected<void, SteamError> StoreStats() override
    {
        return Checked(SteamUserStats()->StoreStats());
    }

    [[nodiscard]] std::expected<void, SteamError> SetRichPresence(std::string_view key,
                                                                  std::string_view value) override
    {
        return Checked(SteamFriends()->SetRichPresence(Terminated(key).c_str(), Terminated(value).c_str()));
    }

    [[nodiscard]] Hardware RunningOn() const override { return ToHardware(SteamUtils()->IsRunningOnSteamHardware()); }

    [[nodiscard]] DefaultConfig SuggestedConfig() const override
    {
        return ToDefaultConfig(SteamUtils()->GetSteamHardwareDefaultConfig());
    }

    [[nodiscard]] bool IsRunningUnderProton() const override { return SteamUtils()->IsRunningUnderProton(); }

    void Update() override
    {
        const HSteamPipe pipe = SteamAPI_GetHSteamPipe();
        SteamAPI_ManualDispatch_RunFrame(pipe);
        CallbackMsg_t message{};
        while (SteamAPI_ManualDispatch_GetNextCallback(pipe, &message))
        {
            Handle(message);
            SteamAPI_ManualDispatch_FreeLastCallback(pipe);
        }
    }

  private:
    void Handle(const CallbackMsg_t &message)
    {
        if (message.m_iCallback == GameOverlayActivated_t::k_iCallback)
        {
            const GameOverlayActivated_t *overlay = reinterpret_cast<const GameOverlayActivated_t *>(message.m_pubParam);
            _overlayActive = overlay->m_bActive != 0;
        }
        else if (message.m_iCallback == UserStatsStored_t::k_iCallback)
        {
            const UserStatsStored_t *stored = reinterpret_cast<const UserStatsStored_t *>(message.m_pubParam);
            if (stored->m_eResult != k_EResultOK)
            {
                Core::Log::Warn("Steam: storing stats failed (result {})", static_cast<std::int32_t>(stored->m_eResult));
            }
        }
    }

    bool _overlayActive = false;
};

} // namespace

bool IsBuiltWithSteamworks() noexcept
{
    return true;
}

std::expected<std::unique_ptr<Client>, SteamError> Connect(AppId /*app*/)
{
    SteamErrMsg message{};
    const ESteamAPIInitResult result = SteamAPI_InitEx(&message);
    if (result != k_ESteamAPIInitResult_OK)
    {
        Core::Log::Warn("Steam: {}", message);
        return std::unexpected(ToError(result));
    }
    // After the API starts and before the first frame's dispatch: Steam refuses
    // it any earlier, and then no callback ever arrives.
    SteamAPI_ManualDispatch_Init();
    return std::make_unique<Steamworks>();
}

bool RelaunchThroughSteamIfNeeded(AppId app)
{
    return SteamAPI_RestartAppIfNecessary(app.value);
}

} // namespace Assisi::Steam
