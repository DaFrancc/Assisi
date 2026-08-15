/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Diagnostics.hpp
/// @brief Launch-stamped diagnostic artifacts and their retention.
///
/// One file per launch, named for when the process started, oldest pruned on
/// the next launch. Truncating a single file per run loses the crash it was
/// meant to record, because the player relaunches before sending it.
///
/// Usage:
///   const std::string name = std::format("assisi-{}.log", LaunchStamp());
///   PruneOldFiles(dir, "assisi-", ".log", 5, name);

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Assisi::Core
{

/// @brief The local time zone, resolved once and cached; null if unavailable.
///
/// current_zone() reports missing tz data only by throwing, so the probe is paid
/// once and cached. On failure this stays null and callers format UTC, keeping
/// later calls off a throwing path — they run on any thread and in the crash
/// handler.
const std::chrono::time_zone *LocalZone();

/// @brief This process's launch time and pid, as "YYYYMMDD-HHMMSS-<pid>".
///
/// Constant after the first call, so a run's log and crash report share a stamp
/// and pair by name. No colons (illegal in Windows filenames); ordered fields so
/// a lexicographic sort is chronological, which is what pruning relies on.
///
/// The pid is what makes the name unique. Seconds resolution alone collides
/// whenever two processes start in the same second — a launcher opening a server
/// and a client, a supervisor restarting a crashed server, a double-click — and
/// both would then truncate and interleave into one file, losing a whole run.
const std::string &LaunchStamp();

/// @brief Gives the calling thread its own stack for signal handlers to run on.
///
/// Call once from the entry point of every thread the engine spawns. sigaltstack
/// is per-thread, so installing it on the main thread — which is all a crash
/// handler can do for itself — leaves every worker without one. A stack-overflow
/// SIGSEGV arrives with no usable stack, so a worker that overflows faults again
/// inside the handler and produces no report at all.
///
/// No-op on Windows, which has no equivalent concept (structured exception
/// handling gets its own stack from the OS).
///
/// Threads created by third-party code — Jolt's own pool — are not covered.
void InstallSignalStackForThisThread() noexcept;

/// @brief Deletes the oldest matching files in `dir`, keeping the newest `keep`.
///
/// Matches on `prefix` and `extension`, so only this engine's own artifacts are
/// candidates. "Newest" is by filename: LaunchStamp() names sort chronologically
/// *provided the clock moves forward*, which is exactly what `protect` exists to
/// cover.
///
/// `protect` is a filename that is never deleted and counts toward `keep`. Pass
/// the current run's own artifact. Sorting alone cannot keep it: LaunchStamp()
/// is local time, so a DST fall-back or an NTP step back makes this run's name
/// sort *oldest* and it is pruned first — on POSIX while its descriptor is still
/// open, so the run writes its whole log into an unlinked inode and loses it
/// silently at exit.
///
/// Best-effort: a directory it cannot read is skipped (the normal first-launch
/// case), and a file it cannot delete warns and stays. Failing to tidy up is not
/// worth failing a launch over.
void PruneOldFiles(const std::filesystem::path &dir, std::string_view prefix, std::string_view extension,
                   uint32_t keep, std::string_view protect = {}) noexcept;

} // namespace Assisi::Core
