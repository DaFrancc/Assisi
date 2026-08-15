/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <chrono>
#include <exception>
#include <format>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <Assisi/Core/Diagnostics.hpp>
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

bool HasConsoleOutput()
{
#ifdef _WIN32
    // NULL for a GUI-subsystem process launched without a console.
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
#else
    // fd 1 exists unless deliberately closed; a redirect still counts.
    return fcntl(STDOUT_FILENO, F_GETFD) != -1;
#endif
}

static bool StdoutIsTerminal()
{
#ifdef _WIN32
    // GetConsoleMode succeeds only for a real console handle, failing for a
    // file or pipe — exactly the distinction wanted.
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != FALSE;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

ConsoleSink::ConsoleSink() : _color(StdoutIsTerminal())
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
    // Streamed in pieces rather than formatted into one string, which would
    // allocate a decorated copy of every line to write it once. The caller holds
    // the logger lock, so the pieces cannot interleave — except under Fatal,
    // which try-locks and accepts that by design.
    if (_color)
    {
        std::cout << LevelColor(level) << message << Reset << '\n';
    }
    else
    {
        std::cout << message << '\n';
    }
}

// -------------------------------------------------------------------------
// FileSink
// -------------------------------------------------------------------------

namespace
{
// Local time of day, millisecond precision ("14:23:45.123"); UTC if no zone
// data. No date — the filename already carries the launch date.
std::string Timestamp()
{
    using namespace std::chrono;
    const sys_time<milliseconds> nowUtc = floor<milliseconds>(system_clock::now());
    if (const time_zone *zone = LocalZone())
    {
        return std::format("{:%H:%M:%S}", zone->to_local(nowUtc));
    }
    return std::format("{:%H:%M:%S}", nowUtc);
}
} // namespace

// Truncate, not append: appending accumulates every run forever. One file per
// launch, pruned by Application, keeps it bounded.
FileSink::FileSink(const std::filesystem::path &path) : _file(path, std::ios::trunc)
{
    // A read-only user root — an install under Program Files or /opt with no
    // ASSISI_USER_ROOT set — otherwise produces no log and no explanation for
    // its absence. This sink is not registered yet, so the warning goes to the
    // console sink and cannot recurse into this one.
    if (!_file.is_open())
    {
        Log::Warn("FileSink: could not open {} — this run will leave no log file.", path.string());
    }
}

void FileSink::Write(LogLevel /*level*/, std::string_view message)
{
    if (!_file.is_open())
    {
        return;
    }
    _file << Timestamp() << ' ' << message << '\n';
    // Flush every line: a buffered tail is exactly what a hard crash loses.
    _file.flush();
}

} // namespace Assisi::Core
