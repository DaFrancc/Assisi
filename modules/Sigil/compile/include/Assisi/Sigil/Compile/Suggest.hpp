/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Suggest.hpp
/// @brief The closest name to a misspelt one, for "did you mean" in errors.
///
/// Public so a vocabulary's own checks phrase a typo the same way the compiler
/// does.

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Assisi::Sigil::Compile
{

/// @brief The name in @p candidates closest to @p name, if one is close enough
///        to be a typo of it.
///
/// Close means few single-character edits: an insertion, a deletion, a change,
/// or two neighbours swapped, which is the commonest typo. How many edits count
/// grows with the name's length, so a short name needs a near match.
[[nodiscard]] std::optional<std::string_view> ClosestName(std::span<const std::string_view> candidates,
                                                          std::string_view name);

/// @brief ` — did you mean "x"?` for the closest candidate, or nothing.
[[nodiscard]] std::string DidYouMean(std::span<const std::string_view> candidates, std::string_view name);

} // namespace Assisi::Sigil::Compile
