/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <format>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

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

    std::lock_guard<std::mutex> guard(_mutex);
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

    std::lock_guard<std::mutex> guard(_mutex);
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
