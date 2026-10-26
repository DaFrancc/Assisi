/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CrashReport.hpp
/// @brief Platform crash handlers, the files they leave behind, and the
///        reporter they start.
///
/// Every file a crash leaves is named crash-<LaunchStamp>, pairing by name with
/// the run's log:
///   - `.dmp`, a minidump: every thread's stack, the registers, and the memory
///     the stacks point at. Windows writes it from the crashing process through
///     DbgHelp. On Linux, crashpad_handler, started from beside the executable
///     when the crash happens, reads the dying process from outside and writes
///     it into a folder of its own; the reporter moves it beside the log.
///   - `.txt`, the crash log (Linux): why the process ended, the crashing
///     thread's frames, and the modules they are in, which is everything needed
///     to name the frames afterwards.
///
/// Not a core file: the kernel owns those through core_pattern, and
/// systemd-coredump puts them somewhere we neither choose nor can prune.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace Assisi::App
{

/// @brief The crash log's extension, including the dot.
inline constexpr std::string_view kCrashLogExtension = ".txt";

/// @brief The minidump's extension, including the dot.
inline constexpr std::string_view kCrashDumpExtension = ".dmp";

/// @brief The program beside the executable that writes a Linux minidump.
inline constexpr std::string_view kCrashDumpHandlerName = "crashpad_handler";

/// @brief The folder in the user root that program writes into.
inline constexpr std::string_view kCrashDumpFolderName = "crashpad";

/// @brief What every file a crash leaves is named, without its extension, for
/// the run launched at @p launchStamp: crash-<stamp>.
[[nodiscard]] std::string CrashFileStem(std::string_view launchStamp);

/// @brief Installs the platform crash handlers.
///
/// @p base names every file a crash leaves, without an extension. @p dumps is
/// the folder crashpad_handler writes into on Linux, before the reporter moves
/// the dump to @p base; Windows writes the dump straight to @p base.
///
/// Call once, early. The paths are copied into fixed storage the handler can
/// read without allocating: a handler that builds a string fails when the heap
/// is what broke.
void InstallCrashHandlers(const std::filesystem::path &base, const std::filesystem::path &dumps) noexcept;

/// @brief Has the crash handlers start @p command after a crash, once the files
/// are written: the program first, then its arguments. Empty starts nothing.
///
/// The command is laid out now, so starting it from a crash allocates nothing.
/// Linux starts it; Windows writes its dump and starts nothing.
void SetCrashReporter(std::span<const std::string> command);

} // namespace Assisi::App
