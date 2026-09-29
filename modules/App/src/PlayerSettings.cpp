/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/PlayerSettings.hpp>

#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Mixer.hpp>
#include <Assisi/Core/Logger.hpp>

#include <optional>
#include <string>
#include <utility>

namespace Assisi::App
{

PlayerSettings::PlayerSettings(OptionsConfig &options, Audio::Mixer *mixer) noexcept
    : _options(options), _mixer(mixer)
{
}

void PlayerSettings::ApplyAudio()
{
    if (_mixer == nullptr)
    {
        return;
    }
    for (const std::pair<const std::string, float> &setting : _options.busVolumes)
    {
        const std::optional<Audio::BusId> bus = _mixer->Layout().FindBus(setting.first);
        if (!bus)
        {
            // Kept in the options: the bus may come back with the next build of the game.
            Core::Log::Info("Audio: the options set a volume for '{}', which this game has no bus for.",
                            setting.first);
            continue;
        }
        _mixer->SetBusVolume(*bus, setting.second);
    }
}

void PlayerSettings::Save() const
{
    _options.SaveToJson();
}

} // namespace Assisi::App
