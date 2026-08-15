/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CrashReport.hpp
/// @brief Platform crash handlers and the artifact they leave behind.
///
///   - Windows: a minidump (.dmp) — thread stacks, globals, and the memory the
///     stacks point at.
///   - POSIX: a text report (.txt) — signal, fault address, si_code, backtrace.
///     Not a core file: the kernel owns those through core_pattern, and
///     systemd-coredump puts them somewhere we neither choose nor can prune.
///
/// Both named crash-<LaunchStamp>.<ext>, pairing by name with the run's log.

#include <filesystem>
#include <string_view>

namespace Assisi::App
{

/// @brief Extension for this platform's crash report, including the dot.
/// ".dmp" on Windows, ".txt" elsewhere. Used for naming and for pruning.
[[nodiscard]] std::string_view CrashReportExtension() noexcept;

/// @brief Installs the platform crash handlers, writing to `path` if they fire.
///
/// Call once, early. The path is copied into fixed storage the handler can read
/// without allocating: a handler that builds a string fails when the heap is
/// what broke.
void InstallCrashHandlers(const std::filesystem::path &path) noexcept;

} // namespace Assisi::App
