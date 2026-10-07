/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BusLayout.hpp
/// @brief The tree of buses a mixer is built with: the defaults, then the game's own.

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/BusConfig.hpp>
#include <Assisi/Core/InternedString.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Assisi::Audio
{

/// @brief The buses every game has. Master is the root; the rest sit under it.
enum class DefaultBus : std::uint8_t
{
    Master,
    Music,
    Sfx,
    Ambient,
    Ui,
    Voice,
    Count_,
};

/// @brief Most buses a layout holds, defaults included. Far above what a game
/// declares; it bounds the mixer's bus storage, which never grows.
inline constexpr std::size_t kMaxBuses = 64;

/// @brief One bus of a layout. Default buses have the ids ToBusId gives them.
struct BusId
{
    std::uint16_t index = 0;

    [[nodiscard]] bool operator==(const BusId &) const = default;
};

[[nodiscard]] constexpr BusId ToBusId(DefaultBus bus) noexcept
{
    return BusId{static_cast<std::uint16_t>(std::to_underlying(bus))};
}

/// @brief Every bus's name, parent and starting volume. Fixed once built: a
/// parent always comes before its children, so a layout has no cycles.
class BusLayout
{
public:
    /// @brief The default buses alone.
    [[nodiscard]] static BusLayout Defaults();

    /// @brief The default buses, then @p config's in the order declared.
    [[nodiscard]] static std::expected<BusLayout, AudioError> FromConfig(const BusConfig &config);

    [[nodiscard]] std::optional<BusId> FindBus(std::string_view name) const;
    [[nodiscard]] std::string_view Name(BusId bus) const;

    /// @brief The bus @p bus mixes into; empty for Master.
    [[nodiscard]] std::optional<BusId> Parent(BusId bus) const;

    [[nodiscard]] float DefaultVolume(BusId bus) const;
    [[nodiscard]] std::size_t Count() const noexcept { return _buses.size(); }

private:
    struct Bus
    {
        Core::InternedString name;
        float volume = 1.0f;
        std::optional<BusId> parent;
    };

    std::vector<Bus> _buses;
};

} // namespace Assisi::Audio
