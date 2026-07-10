/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <chrono>
#include <format>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#include <Assisi/Core/Sinks.hpp>

namespace Assisi::Core
{

// -------------------------------------------------------------------------
// Color helpers
// -------------------------------------------------------------------------

static constexpr std::string_view Reset = "\033[0m";

static std::string_view LevelColor(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "\033[35m"; // magenta
    case LogLevel::Debug:
        return "\033[36m"; // cyan
    case LogLevel::Info:
        return "\033[97m"; // bright white
    case LogLevel::Warn:
        return "\033[33m"; // yellow
    case LogLevel::Error:
        return "\033[31m"; // red
    case LogLevel::Fatal:
        return "\033[1;31m"; // bold red
    }
    return "";
}

// -------------------------------------------------------------------------
// ConsoleSink
// -------------------------------------------------------------------------

ConsoleSink::ConsoleSink()
{
#ifdef _WIN32
    auto enableAnsi = [](DWORD stdHandle)
    {
        HANDLE handle = GetStdHandle(stdHandle);
        DWORD mode = 0;
        if (GetConsoleMode(handle, &mode))
        {
            SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    };
    enableAnsi(STD_OUTPUT_HANDLE);
    enableAnsi(STD_ERROR_HANDLE);
#endif
}

void ConsoleSink::Write(LogLevel level, std::string_view message)
{
    // Everything goes to stdout so output order is preserved.
    // Colors already differentiate severity clearly.
    std::cout << std::format("{}{}{}\n", LevelColor(level), message, Reset);
}

// -------------------------------------------------------------------------
// FileSink
// -------------------------------------------------------------------------

namespace
{
// Wall-clock time of day, millisecond precision (e.g. "14:23:45.123"). No date:
// the file is truncated per run, so a single run is unlikely to span midnight,
// and the run's start is on the file's own mtime anyway.
std::string Timestamp()
{
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return std::format("{:%H:%M:%S}", now);
}
} // namespace

// Truncate, not append: an append-mode log accumulates every run forever (that
// was the multi-MB assisi.log). One run per file keeps it bounded.
FileSink::FileSink(const std::filesystem::path &path) : _file(path, std::ios::trunc)
{
}

void FileSink::Write(LogLevel /*level*/, std::string_view message)
{
    if (!_file.is_open())
    {
        return;
    }
    _file << Timestamp() << ' ' << message << '\n';
    // Flush every line: only a handful per run, and the point of a file log is
    // to survive the crash that a buffered tail would otherwise be lost to.
    _file.flush();
}

} // namespace Assisi::Core
