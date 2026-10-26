/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Abort.hpp
/// @brief Ending the process on purpose, in every build.
///
/// For a condition nothing can carry on past, in a shipped build as much as a
/// debug one: content that needs code this build does not have, a package that
/// is corrupt, a device without something the renderer cannot do without.
/// ASSISI_ASSERT is the wrong tool for those: it checks a programming invariant
/// and compiles to nothing in release, which is the build where they happen.
///
/// Abort logs the reason at Fatal, keeps a copy where a crash handler can read
/// it, and aborts. The abort is caught by the same handlers as any crash, so a
/// deliberate end leaves the same log line, crash report and dump, and opens the
/// same window, that a crash does.

#include <Assisi/Core/Logger.hpp>

#include <cstdint>
#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Assisi::Core
{

/// @brief Logs @p reason as having happened at @p file : @p line, keeps it for
/// AbortReason, and aborts.
///
/// The form for a caller that has a location of its own to report, as a failed
/// ASSISI_ASSERT does. Everything else calls Abort.
[[noreturn]] void AbortAt(std::string_view reason, std::string_view file, std::uint32_t line) noexcept;

/// @brief Ends the process with a reason, in every build.
///
/// Example: Core::Abort("The package {} is corrupt", path.string());
template <typename... Args>
[[noreturn]] void Abort(LocFmtStr<std::type_identity_t<Args>...> fmtLoc, Args &&... args)
{
    AbortAt(std::format(fmtLoc.fmt, std::forward<Args>(args)...), fmtLoc.loc.file_name(), fmtLoc.loc.line());
}

/// @brief Why the process is ending, as the first Abort gave it, or empty when
/// nothing has aborted.
///
/// Safe to read from a signal handler: the text is in fixed storage, written
/// once and published before the abort is raised.
[[nodiscard]] const char *AbortReason() noexcept;

/// @brief Where that Abort was called from, as "file:line", or empty.
[[nodiscard]] const char *AbortLocation() noexcept;

} // namespace Assisi::Core
