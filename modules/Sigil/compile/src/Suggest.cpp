/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <vector>

namespace Assisi::Sigil::Compile
{

namespace
{

/// A name this many characters long allows one more edit, so "walk" allows one
/// and "locomotion" three.
constexpr std::size_t kCharactersPerEdit = 3;

/// The fewest single-character edits from @p from to @p to, counting a swap of
/// two neighbours as one.
std::size_t EditDistance(std::string_view from, std::string_view to)
{
    const std::size_t columns = to.size() + 1;
    std::vector<std::size_t> table((from.size() + 1) * columns, 0);
    for (std::size_t i = 0; i <= from.size(); ++i)
    {
        table[i * columns] = i;
    }
    for (std::size_t j = 0; j <= to.size(); ++j)
    {
        table[j] = j;
    }
    for (std::size_t i = 1; i <= from.size(); ++i)
    {
        for (std::size_t j = 1; j <= to.size(); ++j)
        {
            const std::size_t change = from[i - 1] == to[j - 1] ? 0 : 1;
            std::size_t best = std::min({table[(i - 1) * columns + j] + 1, table[i * columns + j - 1] + 1,
                                         table[(i - 1) * columns + j - 1] + change});
            const bool swapped = i > 1 && j > 1 && from[i - 1] == to[j - 2] && from[i - 2] == to[j - 1];
            if (swapped)
            {
                best = std::min(best, table[(i - 2) * columns + j - 2] + 1);
            }
            table[i * columns + j] = best;
        }
    }
    return table[from.size() * columns + to.size()];
}

} // namespace

std::optional<std::string_view> ClosestName(std::span<const std::string_view> candidates, std::string_view name)
{
    const std::size_t allowed = std::max<std::size_t>(1, name.size() / kCharactersPerEdit);
    std::optional<std::string_view> closest;
    std::size_t closestDistance = std::numeric_limits<std::size_t>::max();
    for (const std::string_view candidate : candidates)
    {
        if (candidate == name)
        {
            continue;
        }
        const std::size_t distance = EditDistance(name, candidate);
        if (distance <= allowed && distance < closestDistance)
        {
            closest = candidate;
            closestDistance = distance;
        }
    }
    return closest;
}

std::string DidYouMean(std::span<const std::string_view> candidates, std::string_view name)
{
    const std::optional<std::string_view> closest = ClosestName(candidates, name);
    if (!closest.has_value())
    {
        return {};
    }
    return std::format(" — did you mean \"{}\"?", *closest);
}

} // namespace Assisi::Sigil::Compile
