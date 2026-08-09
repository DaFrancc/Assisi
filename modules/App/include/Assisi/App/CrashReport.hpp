/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CrashReport.hpp
/// @brief Platform crash handlers and the artifact they leave behind.
///
/// What gets written differs by platform, and the difference is not cosmetic:
///
///   - Windows: a real minidump (.dmp), loadable in a debugger, with thread
///     stacks, globals, and the memory the stacks point at.
///   - POSIX: a text report (.txt) — signal, fault address, registers, and a
///     symbolized backtrace. A process cannot write its own core file; the
///     kernel owns that through core_pattern, and on a systemd machine
///     systemd-coredump takes it somewhere we neither choose nor can prune. So
///     we write the thing we *can* control, which for triage is often the
///     faster artifact anyway.
///
/// Both are named crash-<LaunchStamp>.<ext>, so a report pairs by name with the
/// log from the same run.

#include <filesystem>
#include <string_view>

namespace Assisi::App
{

/// @brief Extension for this platform's crash report, including the dot.
/// ".dmp" on Windows, ".txt" elsewhere. Used for naming and for pruning.
[[nodiscard]] std::string_view CrashReportExtension() noexcept;

/// @brief Installs the platform crash handlers, writing to `path` if they fire.
///
/// Call once, early, before anything that could crash. The path is copied into
/// storage the handler can read without allocating — a handler that has to build
/// a string is a handler that fails when the heap is what broke.
void InstallCrashHandlers(const std::filesystem::path &path) noexcept;

} // namespace Assisi::App
