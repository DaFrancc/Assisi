/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Sinks.hpp
/// @brief Built-in log sinks for the Assisi logging system.
///
/// Available sinks:
///   - ConsoleSink  — writes colored output to stdout
///   - FileSink     — writes to a file, truncated at startup, timestamped
///
/// Future sinks (not yet implemented):
///   - ScreenSink   — in-game console overlay

#include <filesystem>
#include <fstream>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

/// @brief Whether the process has anywhere for console output to go.
///
/// False in a shipped GUI build on Windows, which has no console attached: a
/// ConsoleSink there would format every line and write it to a handle no one
/// can read. On POSIX stdout essentially always exists, though it may be a
/// redirect to a file or /dev/null rather than a terminal — which is a
/// legitimate place to log, so it still counts.
///
/// Install ConsoleSink only when this is true.
[[nodiscard]] bool HasConsoleOutput();

/// @brief Writes log messages to stdout, colored when stdout is a terminal.
///
/// Enables ANSI virtual terminal processing on Windows automatically.
/// Every level goes to stdout, including Error and Fatal: splitting them across
/// two streams lets the console interleave them out of order, which costs more
/// than it buys when the color already marks severity.
///
/// Color is emitted only when stdout is an actual terminal. Redirect the output
/// and the escape sequences would otherwise end up in whatever file or pipe is
/// on the other side, where they are noise rather than color.
struct ConsoleSink : Sink
{
    /// @brief Detects terminal support and enables ANSI color on Windows.
    ConsoleSink();

    void Write(LogLevel level, std::string_view message) override;

  private:
    bool _color = false;
};

/// @brief Writes timestamped log messages to a file.
///
/// The file is truncated when the sink opens (one run per file), so it can't
/// grow unbounded across runs. Each line is prefixed with a wall-clock
/// timestamp and flushed immediately — the log's job is post-mortem, and an
/// unflushed tail is exactly what a hard crash would otherwise lose.
struct FileSink : Sink
{
    /// @brief Opens the file at the given path, truncating any previous contents.
    explicit FileSink(const std::filesystem::path &path);

    void Write(LogLevel level, std::string_view message) override;

  private:
    std::ofstream _file;
};

} // namespace Assisi::Core
