/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LaunchArgs.hpp
/// @brief Argument shapes both entry points parse.
///
/// The Game and the GameEditor take different verbs — a shipped game has no
/// level flag and no capture harness — but the pieces a verb is spelled out of
/// are the same, and a second parse of "1920x1080" that disagreed with the first
/// would be a bug nobody goes looking for.
///
/// Each returns false on a malformed value and prints nothing: the caller owns
/// the message, because only it knows which flag it was reading and what its
/// usage text looks like.

#include <cstdint>
#include <array>
#include <string>
#include <string_view>

namespace Game
{

/// @brief Parses a strictly positive integer.
///
/// Zero is rejected along with negatives: a capture of no frames and a capture
/// nobody asked for are different intentions, and the flag's presence already
/// expressed one of them.
[[nodiscard]] bool ParsePositive(std::string_view text, std::int32_t &out);

/// @brief Parses "<width>x<height>". Both halves must be positive.
///
/// Anything else is a typo worth refusing rather than silently rendering at some
/// other size and publishing the number under the resolution that was asked for.
[[nodiscard]] bool ParseResolution(std::string_view text, std::int32_t &width, std::int32_t &height);

/// @brief Parses "ex,ey,ez,tx,ty,tz" into a camera eye and target.
[[nodiscard]] bool ParseCameraPose(std::string_view text, std::array<float, 3> &eye,
                                   std::array<float, 3> &target);

/// @brief Parses "addr", "addr:port", or ":port", leaving whichever half it does
/// not find untouched.
///
/// IPv6 literals are not handled: the plain form is what a connect flag takes,
/// and anything more elaborate belongs in a server browser rather than in argv.
[[nodiscard]] bool ParseAddress(std::string_view text, std::string &outAddress, std::uint16_t &outPort);

} // namespace Game
