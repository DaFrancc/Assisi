/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file SystemSearch.cpp

#include <Assisi/Editor/SystemSearch.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace Assisi::Editor
{
namespace
{

std::string ToLower(std::string_view text)
{
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

} // namespace

std::vector<SystemSearchHit> RankSystemMatches(std::span<const std::string_view> candidates,
                                               std::string_view query)
{
    std::vector<SystemSearchHit> hits;
    const std::string queryLower = ToLower(query);
    if (queryLower.empty())
        return hits;

    for (const std::string_view candidate : candidates)
    {
        const std::size_t position = ToLower(candidate).find(queryLower);
        if (position == std::string::npos)
            continue;
        hits.push_back(SystemSearchHit{.name = candidate, .position = position});
    }

    // Stable, so candidates that tie on both keys stay in the order they arrived —
    // the registration order the panel's list below is also drawn in.
    std::stable_sort(hits.begin(), hits.end(),
                     [](const SystemSearchHit &a, const SystemSearchHit &b)
        {
            if (a.position != b.position)
                return a.position < b.position;
            return a.name.size() < b.name.size();
        });
    return hits;
}

} // namespace Assisi::Editor
