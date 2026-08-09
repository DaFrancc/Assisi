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

/// @brief Writes colored log messages to stdout.
///
/// Enables ANSI virtual terminal processing on Windows automatically.
/// Every level goes to stdout, including Error and Fatal: splitting them across
/// two streams lets the console interleave them out of order, which costs more
/// than it buys when the color already marks severity.
struct ConsoleSink : Sink
{
    /// @brief Enables ANSI color support on Windows.
    ConsoleSink();

    void Write(LogLevel level, std::string_view message) override;
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
