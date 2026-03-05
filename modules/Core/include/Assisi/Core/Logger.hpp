#pragma once

/// @file Logger.hpp
/// @brief Assisi logging system.
///
/// Usage:
///   Assisi::Core::Log::Info("Loaded {} assets", count);
///   Assisi::Core::Log::Error("Failed to open file: {}", path);  // captures file/line automatically
///
/// Configure the global logger at startup:
///   Assisi::Core::GetLogger().AddSink(std::make_shared<Assisi::Core::ConsoleSink>());

#include <format>
#include <memory>
#include <source_location>
#include <string_view>
#include <vector>

namespace Assisi::Core
{

// -------------------------------------------------------------------------
// Log level
// -------------------------------------------------------------------------

enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

// -------------------------------------------------------------------------
// Sink interface
// -------------------------------------------------------------------------

/// @brief Abstract output destination for log messages.
///
/// Implement this to add custom sinks (e.g. an in-game console overlay).
/// Built-in sinks are provided in Sinks.hpp.
struct Sink
{
    virtual ~Sink() = default;
    virtual void Write(LogLevel level, std::string_view message) = 0;
};

// -------------------------------------------------------------------------
// Logger
// -------------------------------------------------------------------------

struct Logger
{
    /// @brief Adds an output sink. Multiple sinks can be active simultaneously.
    void AddSink(std::shared_ptr<Sink> sink);

    /// @brief Sets the minimum level — messages below this level are discarded.
    void SetMinLevel(LogLevel level);

    /// @brief Logs a message without source location (Trace–Warn).
    void Log(LogLevel level, std::string_view message);

    /// @brief Logs a message with source location (Error, Fatal).
    void Log(LogLevel level, std::source_location loc, std::string_view message);

  private:
    std::vector<std::shared_ptr<Sink>> _sinks;
    LogLevel _minLevel = LogLevel::Trace;
};

/// @brief Returns the global logger instance.
Logger &GetLogger();

// -------------------------------------------------------------------------
// LocFmtStr — source_location + format string helper
// -------------------------------------------------------------------------

/// @brief Bundles a format string with the call-site source location.
///
/// When Error() or Fatal() are called with a string literal, this type is
/// implicitly constructed, capturing source_location::current() at the call
/// site — no macros required.
struct LocFmtStr
{
    std::string_view fmt;
    std::source_location loc;

    LocFmtStr(const char *s, std::source_location loc = std::source_location::current()) : fmt(s), loc(loc) {}

    LocFmtStr(std::string_view s, std::source_location loc = std::source_location::current()) : fmt(s), loc(loc) {}
};

// -------------------------------------------------------------------------
// Free functions
// -------------------------------------------------------------------------

namespace Log
{

template <typename... Args> void Trace(std::format_string<Args...> fmt, Args &&...args);
{
    GetLogger().Log(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void Debug(std::format_string<Args...> fmt, Args &&...args)
{
    GetLogger().Log(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void Info(std::format_string<Args...> fmt, Args &&...args)
{
    GetLogger().Log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void Warn(std::format_string<Args...> fmt, Args &&...args)
{
    GetLogger().Log(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

/// @brief Logs an error with automatic file/line capture.
///
/// Example: Log::Error("Entity {} not found", id);
template <typename... Args> void Error(LocFmtStr fmtLoc, Args &&...args)
{
    GetLogger().Log(LogLevel::Error, fmtLoc.loc, std::vformat(fmtLoc.fmt, std::make_format_args(args...)));
}

/// @brief Logs a fatal error with automatic file/line capture.
///
/// Example: Log::Fatal("Unrecoverable state: {}", reason);
template <typename... Args> void Fatal(LocFmtStr fmtLoc, Args &&...args)
{
    GetLogger().Log(LogLevel::Fatal, fmtLoc.loc, std::vformat(fmtLoc.fmt, std::make_format_args(args...)));
}

} // namespace Log
} // namespace Assisi::Core
