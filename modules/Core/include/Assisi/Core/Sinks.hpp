#pragma once

/// @file Sinks.hpp
/// @brief Built-in log sinks for the Assisi logging system.
///
/// Available sinks:
///   - ConsoleSink  — writes colored output to stdout/stderr
///   - FileSink     — appends to a file
///
/// Future sinks (not yet implemented):
///   - ScreenSink   — in-game console overlay

#include <filesystem>
#include <fstream>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

/// @brief Writes colored log messages to stdout/stderr.
///
/// Enables ANSI virtual terminal processing on Windows automatically.
/// Error and Fatal go to stderr; all other levels go to stdout.
struct ConsoleSink : Sink
{
    /// @brief Enables ANSI color support on Windows.
    ConsoleSink();

    void Write(LogLevel level, std::string_view message) override;
};

/// @brief Appends log messages to a file.
struct FileSink : Sink
{
    /// @brief Opens (or creates) the file at the given path in append mode.
    explicit FileSink(const std::filesystem::path &path);

    void Write(LogLevel level, std::string_view message) override;

  private:
    std::ofstream _file;
};

} // namespace Assisi::Core
