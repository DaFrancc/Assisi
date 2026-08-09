/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <format>
#include <mutex>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

// Fatal must not block: it runs from the crash handler, where the lock may be
// held by the thread that just died. Takes the lock if free, writes anyway if
// not — an interleaved line beats a hung process and no diagnostic. That path
// also reads _sinks unsynchronized, which holds because sinks are added once in
// Application's constructor and never after. Every other level waits its turn.
static std::unique_lock<std::mutex> AcquireForWrite(std::mutex &mutex, LogLevel level)
{
    if (level == LogLevel::Fatal)
    {
        return std::unique_lock<std::mutex>(mutex, std::try_to_lock);
    }
    return std::unique_lock<std::mutex>(mutex);
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

// Backstop only. The Log:: free functions filter before formatting, which is
// where it saves anything; this catches a direct Log() call.
void Logger::Log(LogLevel level, std::string_view message)
{
    if (!IsEnabled(level))
    {
        return;
    }

    const std::unique_lock<std::mutex> lock = AcquireForWrite(_mutex, level);
    std::string line = std::format("{} {}", LevelPrefix(level), message);
    for (std::shared_ptr<Sink> &sink : _sinks)
    {
        sink->Write(level, line);
    }
}

void Logger::Log(LogLevel level, std::source_location loc, std::string_view message)
{
    if (!IsEnabled(level))
    {
        return;
    }

    const std::unique_lock<std::mutex> lock = AcquireForWrite(_mutex, level);
    std::string line = std::format("{} {}({}): {}", LevelPrefix(level), loc.file_name(), loc.line(), message);
    for (std::shared_ptr<Sink> &sink : _sinks)
    {
        sink->Write(level, line);
    }
}

Logger &GetLogger()
{
    static Logger instance;
    return instance;
}

} // namespace Assisi::Core
