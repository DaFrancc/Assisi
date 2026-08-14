/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Sinks.hpp
/// @brief Built-in log sinks for the Assisi logging system.
///
///   - ConsoleSink — stdout, colored when stdout is a terminal
///   - FileSink    — timestamped lines, flushed per line
///   - ScreenSink  — in-game console overlay (planned)

#include <filesystem>
#include <fstream>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

/// @brief Whether the process has anywhere for console output to go.
///
/// False in a shipped GUI build on Windows, which has no console: a ConsoleSink
/// there formats every line for a handle nobody can read. On POSIX stdout
/// essentially always exists, redirects included. Gate ConsoleSink on this.
[[nodiscard]] bool HasConsoleOutput();

/// @brief Writes to stdout, colored when stdout is a terminal.
///
/// Enables ANSI virtual terminal processing on Windows. Every level goes to
/// stdout, Error and Fatal included — two streams reorder against each other,
/// and color already marks severity. Color is suppressed when stdout is not a
/// terminal, so a redirect collects text rather than escape sequences.
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
/// Each line is flushed immediately: the log's job is post-mortem, and an
/// unflushed tail is what a hard crash loses. One file per launch — Application
/// names it from Core::LaunchStamp() and prunes old ones.
struct FileSink : Sink
{
    /// @brief Opens the file at the given path, truncating any previous contents.
    explicit FileSink(const std::filesystem::path &path);

    void Write(LogLevel level, std::string_view message) override;

private:
    std::ofstream _file;
};

} // namespace Assisi::Core
