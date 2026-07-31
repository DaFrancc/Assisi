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

} // namespace Assisi::Chiara
