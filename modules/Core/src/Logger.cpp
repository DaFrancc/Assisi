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
    _sinks.push_back(std::move(sink));
}

void Logger::SetMinLevel(LogLevel level)
{
    _minLevel = level;
}

void Logger::Log(LogLevel level, std::string_view message)
{
    if (level < _minLevel)
    {
        return;
    }

    auto line = std::format("{} {}", LevelPrefix(level), message);
    for (auto &sink : _sinks)
    {
        sink->Write(level, line);
    }
}

void Logger::Log(LogLevel level, std::source_location loc, std::string_view message)
{
    if (level < _minLevel)
    {
        return;
    }

    auto line = std::format("{} {}({}): {}", LevelPrefix(level), loc.file_name(), loc.line(), message);
    for (auto &sink : _sinks)
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
