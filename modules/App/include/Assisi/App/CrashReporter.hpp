/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CrashReporter.hpp
/// @brief What a crash starts: the crashed run's files put in order, and a
///        window saying what happened.
///
/// A crash relaunches the executable that crashed with kCrashReportFlag and the
/// crashed run's launch stamp (CrashReporterCommand). That process is the
/// reporter. It:
///   - moves the minidump crashpad_handler wrote beside the crash log,
///   - names the crash log's frames, function and file:line, wherever the
///     module a frame is in carries symbols, and writes them into the log,
///   - shows ui/Crash.amdn: why the run ended, the stack, and where the files
///     are, with Open folder and Close.
///
/// Nothing is sent anywhere.

#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::App
{

/// @brief Starts a process as a crashed run's reporter; the run's launch stamp
/// follows it.
inline constexpr std::string_view kCrashReportFlag = "--crash-report";

/// @brief The crash window, in the content every game ships.
inline constexpr std::string_view kCrashScreenPath = "ui/Crash.amdn";

/// @brief What a crash in this run starts: this executable, as the reporter for
/// this run, headless if this run is. Empty when the executable cannot find
/// itself.
[[nodiscard]] std::vector<std::string> CrashReporterCommand(bool headless);

/// @brief Moves the newest minidump under @p dumps, crashpad_handler's folder,
/// to @p to. False, logged, when there is none or it cannot be moved.
bool ClaimCrashDump(const std::filesystem::path &dumps, const std::filesystem::path &to);

/// @brief One frame of a crash, as the reporter names it.
struct NamedFrame
{
    /// The function, demangled, or empty when the module has no symbols.
    std::string function;

    /// The source file and line, or empty.
    std::string location;

    /// The file the frame is in.
    std::string module;

    /// Where in that module, as the address the module was linked to run at,
    /// which is what `addr2line -e <module>` reads, so a frame can still be
    /// named afterwards from a copy of the module that has symbols.
    std::uint64_t offset = 0;
};

/// @brief The frames of the crash log @p log, outermost handlers left off, each
/// named as far as its module's symbols allow.
///
/// Names come from addr2line. Without it, or without symbols, a frame is its
/// module and offset, which is what naming it later needs.
[[nodiscard]] std::vector<NamedFrame> NameFrames(std::string_view log);

/// @brief Writes @p frames into @p log, above the raw frames, as the section a
/// reader looks at first. False, logged, when the log cannot be rewritten.
bool WriteStack(const std::filesystem::path &log, const std::vector<NamedFrame> &frames);

/// @brief What a reporter was started for.
struct CrashReportLaunch
{
    /// The crashed run's launch stamp.
    std::string stamp;

    /// The package the window is read from. Empty reads the source asset root,
    /// with whatever readers main() installed, as the editor does.
    std::filesystem::path pak;

    /// Put the files in order and show nothing.
    bool headless = false;
};

/// @brief The reporter launch @p arguments, a main()'s argv, ask for: the stamp
/// after kCrashReportFlag, which must come first, and `--headless` after it.
/// Empty when they do not start a reporter.
[[nodiscard]] std::optional<CrashReportLaunch> CrashReportLaunchFrom(std::span<char *const> arguments);

/// @brief Puts the crashed run's files in order, then shows the crash window
/// until it is closed.
///
/// Returns the exit status: success once the files are in order, whether or
/// not a window could be shown.
[[nodiscard]] std::int32_t RunCrashReporter(const CrashReportLaunch &launch);

/// @brief Pushed by the crash window's Open folder button.
AEVENT()
struct OpenCrashFolder
{
};

} // namespace Assisi::App
