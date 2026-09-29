/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/BusLayout.hpp>

#include <array>

namespace Assisi::Audio
{

namespace
{

/// @brief The default buses' names, in DefaultBus order.
constexpr std::array<std::string_view, std::to_underlying(DefaultBus::Count)> kDefaultBusNames{
    "Master", "Music", "SFX", "Ambient", "UI", "Voice",
};

} // namespace

BusLayout BusLayout::Defaults()
{
    BusLayout layout;
    layout._buses.reserve(kDefaultBusNames.size());
    for (const std::string_view name : kDefaultBusNames)
    {
        const bool isMaster = layout._buses.empty();
        layout._buses.push_back(Bus{
                .name   = Core::InternedString{name},
                .volume = 1.0f,
                .parent = isMaster ? std::nullopt : std::optional<BusId>{ToBusId(DefaultBus::Master)},
            });
    }
    return layout;
}

std::expected<BusLayout, AudioError> BusLayout::FromConfig(const BusConfig &config)
{
    BusLayout layout = Defaults();
    if (layout._buses.size() + config.buses.size() > kMaxBuses)
    {
        return std::unexpected(AudioError::TooManyBuses);
    }

    for (const BusDeclaration &declared : config.buses)
    {
        if (layout.FindBus(declared.name.View()).has_value())
        {
            return std::unexpected(AudioError::DuplicateBus);
        }
        // Only buses already in the layout can be parents, which is what keeps it free of cycles.
        const std::optional<BusId> parent = layout.FindBus(declared.parent.View());
        if (!parent.has_value())
        {
            return std::unexpected(AudioError::UnknownParentBus);
        }
        layout._buses.push_back(Bus{.name = declared.name, .volume = declared.volume, .parent = parent});
    }
    return layout;
}

std::optional<BusId> BusLayout::FindBus(std::string_view name) const
{
    for (std::size_t i = 0; i < _buses.size(); ++i)
    {
        if (_buses[i].name.View() == name)
        {
            return BusId{static_cast<std::uint16_t>(i)};
        }
    }
    return std::nullopt;
}

std::string_view BusLayout::Name(BusId bus) const
{
    return _buses.at(bus.index).name.View();
}

std::optional<BusId> BusLayout::Parent(BusId bus) const
{
    return _buses.at(bus.index).parent;
}

float BusLayout::DefaultVolume(BusId bus) const
{
    return _buses.at(bus.index).volume;
}

} // namespace Assisi::Audio
