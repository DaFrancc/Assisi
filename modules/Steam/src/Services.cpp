/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Steam/Steam.hpp>

namespace Assisi::Steam
{
namespace
{

/// The read, add and write AddToStat is, for either kind of stat.
template <typename Number, typename Read>
std::expected<Number, SteamError> Add(Services &steam, std::string_view name, Number amount, Read read)
{
    const std::expected<Number, SteamError> current = read(name);
    if (!current)
    {
        return std::unexpected(current.error());
    }
    const Number updated = *current + amount;
    if (const std::expected<void, SteamError> written = steam.SetStat(name, updated); !written)
    {
        return std::unexpected(written.error());
    }
    return updated;
}

} // namespace

std::expected<std::int32_t, SteamError> Services::AddToStat(std::string_view name, std::int32_t amount)
{
    return Add(*this, name, amount, [this](std::string_view stat) { return StatInt(stat); });
}

std::expected<float, SteamError> Services::AddToStat(std::string_view name, float amount)
{
    return Add(*this, name, amount, [this](std::string_view stat) { return StatFloat(stat); });
}

} // namespace Assisi::Steam
