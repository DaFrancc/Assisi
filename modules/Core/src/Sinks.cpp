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
    // A GUI-subsystem process launched without a console gets NULL here. That
    // is the case worth catching: a shipped game would otherwise format every
    // line and hand it to a handle that cannot take it.
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
#else
    // fd 1 exists unless someone deliberately closed it. It may well be a
    // redirect rather than a terminal, which is still somewhere worth writing.
    return fcntl(STDOUT_FILENO, F_GETFD) != -1;
#endif
}

// Color belongs to terminals. Piped or redirected output gets the escape
// sequences embedded in it as literal bytes, which is how a log file ends up
// full of \033[97m.
static bool StdoutIsTerminal()
{
#ifdef _WIN32
    // GetConsoleMode succeeds only for a real console handle — it fails when
    // stdout is a file or a pipe, which is exactly the distinction we want.
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
    // Everything goes to stdout so output order is preserved.
    // Colors already differentiate severity clearly.
    //
    // Streamed in pieces rather than std::format'd into one string: the
    // formatted version allocated a whole decorated copy of every line just to
    // write it once. These inserters write the parts straight through. The
    // caller normally holds the logger lock, so the pieces cannot interleave —
    // the exception is Fatal, which try-locks and may write unlocked, and which
    // accepts an interleaved line by design.
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
// Local wall-clock time of day, millisecond precision (e.g. "14:23:45.123"),
// via the cached zone (UTC if no zone data was available). No date: the file is
// truncated per run, so a single run is unlikely to span midnight, and the
// run's start is on the file's own mtime anyway.
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
