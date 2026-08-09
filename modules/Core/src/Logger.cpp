/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <format>
#include <mutex>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

// Fatal never blocks. It is normally the last thing the process does, and it
// runs from the crash handler — where the lock may be held by the very thread
// that just died, and where blocking hangs the process and loses the
// diagnostic entirely. So Fatal takes the lock if it is free and writes anyway
// if it is not: at worst one interleaved line, in a log nobody reads past this
// point. That is the whole ambition — attempt to preserve the message, and give
// up gracefully rather than defend the integrity of a process already lost.
//
// The unlocked path also reads _sinks without synchronization. Sinks are added
// once during Application's constructor and never afterward, so there is no
// writer to race; a Fatal concurrent with AddSink would be the exception, and
// is accepted on the same grounds.
//
// Every other level waits its turn exactly as before.
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

// The IsEnabled() checks below are a backstop, not the primary filter — the
// Log:: free functions already checked before formatting, which is the point at
// which the check saves anything. These only matter for a direct Log() call.
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
