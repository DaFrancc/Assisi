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
        case LogLevel::Trace: return "\033[35m";   // magenta
        case LogLevel::Debug: return "\033[36m";   // cyan
        case LogLevel::Info:  return "\033[97m";   // bright white
        case LogLevel::Warn:  return "\033[33m";   // yellow
        case LogLevel::Error: return "\033[31m";   // red
        case LogLevel::Fatal: return "\033[1;31m"; // bold red
    }
    return "";
}

// -------------------------------------------------------------------------
// ConsoleSink
// -------------------------------------------------------------------------

ConsoleSink::ConsoleSink()
{
#ifdef _WIN32
    auto enableAnsi = [](DWORD handle)
    {
        HANDLE h = GetStdHandle(handle);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
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

FileSink::FileSink(const std::filesystem::path& path)
    : _file(path, std::ios::app)
{
}

void FileSink::Write(LogLevel /*level*/, std::string_view message)
{
    if (_file.is_open())
        _file << message << '\n';
}

} // namespace Assisi::Core
