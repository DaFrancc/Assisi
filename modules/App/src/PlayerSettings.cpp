/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/PlayerSettings.hpp>

#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Mixer.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace Assisi::App
{

PlayerSettings::PlayerSettings(OptionsConfig &options, Audio::Mixer *mixer) noexcept
    : _options(options), _mixer(mixer)
{
}

std::vector<std::string_view> PlayerSettings::Buses() const
{
    std::vector<std::string_view> names;
    if (_mixer == nullptr)
    {
        return names;
    }
    const Audio::BusLayout &layout = _mixer->Layout();
    names.reserve(layout.Count());
    for (std::size_t i = 0; i < layout.Count(); ++i)
    {
        names.push_back(layout.Name(Audio::BusId{static_cast<std::uint16_t>(i)}));
    }
    return names;
}

std::optional<float> PlayerSettings::BusVolume(std::string_view bus) const
{
    if (_mixer == nullptr)
    {
        return std::nullopt;
    }
    const std::optional<Audio::BusId> id = _mixer->Layout().FindBus(bus);
    if (!id)
    {
        return std::nullopt;
    }
    const std::map<std::string, float, std::less<>>::const_iterator chosen = _options.busVolumes.find(bus);
    if (chosen != _options.busVolumes.end())
    {
        return chosen->second;
    }
    return _mixer->Layout().DefaultVolume(*id);
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
