/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Diagnostics.hpp
/// @brief Launch-stamped diagnostic artifacts and their retention.
///
/// A log or crash report is only useful if it survives long enough to be sent.
/// Truncating one file per run does not survive: the player relaunches after a
/// crash and overwrites the very run that explains it. So each launch writes its
/// own file, named for when the process started, and the oldest are pruned on
/// the next launch.
///
/// Usage:
///   const std::filesystem::path log = dir / std::format("assisi-{}.log", LaunchStamp());
///   PruneOldFiles(dir, "assisi-", ".log", 5);

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Assisi::Core
{

/// @brief The local time zone, resolved once and cached; null if unavailable.
///
/// current_zone() (and the tz database behind it) signals "no zone data" only by
/// throwing — a one-time environment condition — so the probe is paid exactly
/// once and the result cached. On failure this stays null and callers format UTC
/// forever after, which keeps every later call off a throwing path. That matters
/// because these run on any thread and inside the crash handler.
const std::chrono::time_zone *LocalZone();

/// @brief This process's launch time, as "YYYYMMDD-HHMMSS".
///
/// Resolved once on first call and constant thereafter, so the log and the crash
/// report from one run share a stamp and can be paired by name. No colons: they
/// are illegal in Windows filenames. Ordered fields so a lexicographic sort of
/// the directory is a chronological sort, which is what the pruning relies on.
const std::string &LaunchStamp();

/// @brief Deletes the oldest matching files in `dir`, keeping the newest `keep`.
///
/// Matches on `prefix` and `extension` so only this engine's own artifacts are
/// ever considered — never anything else that happens to share the directory.
/// "Newest" is by filename, not mtime: the names are LaunchStamp()s, so they
/// sort chronologically by construction, and a touched file cannot reorder them.
///
/// Best-effort and noexcept. A directory that cannot be read, or a file that
/// cannot be deleted (open in an editor, permissions), warns and is skipped —
/// failing to tidy up is never worth failing a launch over.
void PruneOldFiles(const std::filesystem::path &dir, std::string_view prefix, std::string_view extension,
                   uint32_t keep) noexcept;

} // namespace Assisi::Core
