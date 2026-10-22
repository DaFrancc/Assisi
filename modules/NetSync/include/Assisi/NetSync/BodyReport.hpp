/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BodyReport.hpp
/// @brief What a client says, with its commands, about the bodies it simulates
/// for the host.

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/NetSync/BodyState.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Assisi::NetSync
{

/// @brief Every body a client simulates, as its step of one tick left them.
///
/// Rides in the input packet beside that tick's command, and is repeated in
/// the next packets as the command is, so a lost packet costs nothing.
struct BodyReport
{
    std::vector<BodyState> bodies;
    std::uint64_t tick = 0;
};

/// @brief The most ticks of reports one packet may carry, and bodies one tick
/// may report. Past either, the packet's reports are refused whole.
inline constexpr std::uint32_t kMaxReportTicks = 16;
inline constexpr std::uint32_t kMaxReportedBodies = 64;

/// @brief Writes @p reports: a count, then each tick, its body count and its
/// body records, at the resolutions every body record uses.
void WriteBodyReports(std::span<const BodyReport *const> reports, Core::BitWriter &writer);

/// @brief Inverse of WriteBodyReports, appending to @p out. False on a
/// truncated or malformed section or one past the limits above, having
/// appended nothing.
[[nodiscard]] bool ReadBodyReports(Core::BitReader &reader, std::vector<BodyReport> &out);

} // namespace Assisi::NetSync
