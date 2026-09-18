/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SystemSearch.hpp
/// @brief Ranking behind the Required Systems panel's search field.
///
/// Separate from the panel so the ranking can be tested without an ImGui frame:
/// it is the part with a right answer, and the part an author notices when it is
/// wrong.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace Assisi::Editor
{

/// @brief One candidate that matched, and where in it the match sits.
struct SystemSearchHit
{
    /// Borrowed from the caller's candidate span — valid only as long as it is.
    std::string_view name;
    /// Offset of the first case-insensitive occurrence of the query.
    std::size_t position;
};

/// @brief Case-insensitive substring search over @p candidates, ranked.
///
/// A match nearer the start of a name outranks one further in, so typing `sh`
/// offers `Shadow` before `PreShadow`. Equal positions break by name length —
/// the shorter name is the tighter match — and then by the order @p candidates
/// arrived in, which is registration order and therefore stable between frames.
///
/// An empty @p query matches nothing rather than everything: the field is a
/// filter over a list too long to page through, so an untyped one has no answer
/// worth showing.
[[nodiscard]] std::vector<SystemSearchHit> RankSystemMatches(std::span<const std::string_view> candidates,
                                                             std::string_view query);

} // namespace Assisi::Editor
