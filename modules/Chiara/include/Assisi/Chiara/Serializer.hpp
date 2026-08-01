/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Serializer.hpp
/// @brief Writes a captured window out as a Chrome JSON trace
///        (design: docs/chiara-design-notes.md §6).
///
/// The file opens in **Perfetto** (ui.perfetto.dev) and, because Tracy ships
/// `tracy-import-chrome`, in **Tracy** as well. One exporter, two viewers: the
/// second one covers capture diffing, Perfetto's one real gap, without linking
/// anything into the engine.
///
/// Chrome JSON is the v1 format because it is documented, textual, and read by
/// the widest set of tools. Fuchsia Trace Format is the natural second exporter
/// when size starts to hurt — the event model already holds everything it wants,
/// including raw ticks and a ticks-per-second to convert them by. Perfetto's
/// protobuf is rejected outright: the only thing it would buy is the real
/// frame-timeline UI, which is Android-only.

#include <Assisi/Chiara/Chiara.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace Assisi::Chiara
{

/// @brief What a dump produced. `error` is empty on success.
struct SerializeResult
{
    bool          success            = false;
    std::uint64_t eventsRead         = 0; ///< Records examined across every ring.
    std::uint64_t eventsWritten      = 0; ///< Trace events emitted (one scope can add args, not events).
    std::uint64_t slicesSynthesized  = 0; ///< Scopes still open at dump time, recovered from shadow stacks.
    std::uint64_t orphanedArgs       = 0; ///< Args with no enclosing scope — dropped, never silently.
    std::uint64_t threadsWritten     = 0;
    std::uint64_t bytesWritten       = 0;
    double        windowSeconds      = 0.0; ///< Span actually written.
    double        ticksPerSecond     = 0.0; ///< After refinement against the clock snapshots.
    std::string   error;
};

/// @brief Writes the most recent `lastSeconds` of capture to `path`.
///
/// Pass 0 (the default) for everything the rings still hold.
///
/// Pauses recording for the duration and restores it afterwards, so the capture
/// is blind while it writes — acceptable because a dump happens *after* the
/// interesting part, and required because the ring read protocol only tolerates
/// producers that have stopped. Meant to run on a JobSystem worker so the frame
/// never hitches on it.
#if defined(ASSISI_CHIARA_ENABLED)
[[nodiscard]] SerializeResult SerializeCapture(const std::filesystem::path &path, double lastSeconds = 0.0);
#else
[[nodiscard]] inline SerializeResult SerializeCapture(const std::filesystem::path &, double = 0.0)
{
    SerializeResult result;
    result.error = "Chiara is not compiled in (build with -c, e.g. make gs-c)";
    return result;
}
#endif

/// @brief Progress of a running session.
struct SessionStats
{
    bool          active         = false;
    double        elapsedSeconds = 0.0;
    std::uint64_t eventsWritten  = 0;
    std::uint64_t bytesWritten   = 0;
    std::uint64_t drains         = 0; ///< Times the rings have been emptied to disk.
    std::uint64_t eventsLost     = 0; ///< Overwritten before a drain reached them — see below.
    std::string   path;
};

/// @name Session recording — capture for as long as you like
///
/// The rolling ring answers "what just happened"; a session answers "record this
/// whole thing", where the thing is longer than any buffer would hold. The
/// difference is where the events live: a session streams them to disk as it
/// goes, so its length is bounded by free space rather than by RAM.
///
/// The file is written incrementally and is **valid to open even if the process
/// dies mid-session** — Perfetto accepts a truncated JSON array, so a crash
/// still leaves everything captured up to that moment.
///
/// `eventsLost` is the number that matters while one runs. Draining pauses
/// capture for the length of a ring walk, so it happens on a schedule rather
/// than continuously; if a thread manages to wrap its ring between two drains,
/// the gap is counted and surfaced instead of quietly shortening the trace.
///@{
#if defined(ASSISI_CHIARA_ENABLED)

/// @brief Starts streaming to @p path. Fails if a session is already running.
[[nodiscard]] bool BeginSession(const std::filesystem::path &path);

/// @brief Drains whatever the rings hold into the open session file.
///
/// Call once per frame — it returns immediately unless a ring is filling up, so
/// the cost of asking is a few atomic loads. This is what keeps a session from
/// losing events; nothing else calls it.
void PumpSession();

/// @brief Final drain, then closes the file. Safe to call when none is running.
SerializeResult EndSession();

[[nodiscard]] SessionStats GetSessionStats();

#else

[[nodiscard]] inline bool BeginSession(const std::filesystem::path &) { return false; }
inline void               PumpSession() {}
inline SerializeResult    EndSession() { return {}; }
[[nodiscard]] inline SessionStats GetSessionStats() { return {}; }

#endif
///@}

} // namespace Assisi::Chiara
