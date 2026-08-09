/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <cstdio>
#include <format>
#include <mutex>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

// Where a Fatal goes when the logger lock is unavailable.
//
// The lock can be held by a thread that just died, and blocking there hangs the
// process. But writing to the sinks anyway is not the answer: std::ofstream
// carries no data-race guarantee, so an unlocked FileSink write races the put
// area of a shared filebuf. Measured, that does not merely interleave one line
// — it duplicates and byte-corrupts entries already in the file, which destroys
// the log as evidence.
//
// stdio is thread-safe ([C11 7.21.2]/7), so stderr takes the line without
// touching anything a sink owns. Unbuffered, so it survives the abort that
// follows.
static void WriteToStderr(std::string_view line) noexcept
{
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

static std::string_view LevelPrefix(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return "[TRACE]";
    case LogLevel::Debug:
        return "[DEBUG]";
    case LogLevel::Info:
        return "[INFO ]";
    case LogLevel::Warn:
        return "[WARN ]";
    case LogLevel::Error:
        return "[ERROR]";
    case LogLevel::Fatal:
        return "[FATAL]";
    }
    return "[?????]";
}

void Logger::AddSink(std::shared_ptr<Sink> sink)
{
    std::lock_guard<std::mutex> guard(_mutex);
    _sinks.push_back(std::move(sink));
}

void Logger::SetMinLevel(LogLevel level)
{
    // No lock: the level is atomic, and it guards nothing else.
    _minLevel.store(level, std::memory_order_relaxed);
}

void Logger::Emit(LogLevel level, std::string_view line)
{
    if (level == LogLevel::Fatal)
    {
        std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            WriteToStderr(line);
            return;
        }
        for (std::shared_ptr<Sink> &sink : _sinks)
        {
            sink->Write(level, line);
        }
        return;
    }

    const std::lock_guard<std::mutex> guard(_mutex);
    for (std::shared_ptr<Sink> &sink : _sinks)
    {
        sink->Write(level, line);
    }
}

// Backstop only. The Log:: free functions filter before formatting, which is
// where it saves anything; this catches a direct Log() call.
//
// Formatting happens outside Emit, so the lock covers only the fan-out.
void Logger::Log(LogLevel level, std::string_view message)
{
    if (!IsEnabled(level))
    {
        return;
    }
    Emit(level, std::format("{} {}", LevelPrefix(level), message));
}

void Logger::Log(LogLevel level, std::source_location loc, std::string_view message)
{
    if (!IsEnabled(level))
    {
        return;
    }
    Emit(level, std::format("{} {}({}): {}", LevelPrefix(level), loc.file_name(), loc.line(), message));
}

Logger &GetLogger()
{
    static Logger instance;
    return instance;
}

} // namespace Assisi::Core
