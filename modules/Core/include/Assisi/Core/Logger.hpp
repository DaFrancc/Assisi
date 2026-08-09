/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
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

#include <atomic>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <string_view>
#include <type_traits>
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

/// @brief The global logging service.
///
/// @note Thread-safe. One mutex serializes the Log() fan-out, so concurrent
/// calls neither race on the sink list nor interleave partial lines. AddSink()
/// takes it too; SetMinLevel() does not (the level is atomic). The lock is held
/// across formatting as well as writing — fine at current log volume.
///
/// @note Fatal is the exception: it try-locks and writes regardless, so a crash
/// handler cannot hang on a lock held by the thread that just died, at the cost
/// of a possibly interleaved line. See AcquireForWrite in Logger.cpp.
struct Logger
{
    /// @brief Adds an output sink. Multiple sinks can be active simultaneously.
    void AddSink(std::shared_ptr<Sink> sink);

    /// @brief Sets the minimum level — messages below this level are discarded.
    void SetMinLevel(LogLevel level);

    /// @brief Whether a message at this level would be emitted.
    ///
    /// Also the guard for a call site whose arguments are expensive to build.
    /// Relaxed: the level publishes no other state, so either side of a race
    /// with SetMinLevel is a correct answer.
    [[nodiscard]] bool IsEnabled(LogLevel level) const
    {
        return level >= _minLevel.load(std::memory_order_relaxed);
    }

    /// @brief Logs a message without source location (Trace–Warn).
    void Log(LogLevel level, std::string_view message);

    /// @brief Logs a message with source location (Error, Fatal).
    void Log(LogLevel level, std::source_location loc, std::string_view message);

  private:
    std::mutex _mutex;
    std::vector<std::shared_ptr<Sink>> _sinks;
    std::atomic<LogLevel> _minLevel = LogLevel::Trace;
};

/// @brief Returns the global logger instance.
Logger &GetLogger();

// -------------------------------------------------------------------------
// LocFmtStr — source_location + format string helper
// -------------------------------------------------------------------------

/// @brief Bundles a compile-time format string with the call-site source location.
///
/// When Error() or Fatal() are called with a string literal, this type is
/// implicitly constructed, capturing source_location::current() at the call
/// site — no macros required.
template <typename... Args>
struct LocFmtStr
{
    std::format_string<Args...> fmt;
    std::source_location loc;

    template <typename T>
    consteval LocFmtStr(T &&f, std::source_location sloc = std::source_location::current())
        : fmt(std::forward<T>(f)), loc(sloc)
    {
    }
};

// -------------------------------------------------------------------------
// Free functions
// -------------------------------------------------------------------------

/// @note Each checks the level before formatting: a suppressed message must not
/// pay for a std::format and allocation it will never use.
namespace Log
{

template <typename... Args> void Trace(std::format_string<Args...> fmt, Args &&...args)
{
    Logger &logger = GetLogger();
    if (!logger.IsEnabled(LogLevel::Trace))
    {
        return;
    }
    logger.Log(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void Debug(std::format_string<Args...> fmt, Args &&...args)
{
    Logger &logger = GetLogger();
    if (!logger.IsEnabled(LogLevel::Debug))
    {
        return;
    }
    logger.Log(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void Info(std::format_string<Args...> fmt, Args &&...args)
{
    Logger &logger = GetLogger();
    if (!logger.IsEnabled(LogLevel::Info))
    {
        return;
    }
    logger.Log(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void Warn(std::format_string<Args...> fmt, Args &&...args)
{
    Logger &logger = GetLogger();
    if (!logger.IsEnabled(LogLevel::Warn))
    {
        return;
    }
    logger.Log(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}

/// @brief Logs an error with automatic file/line capture.
///
/// Example: Log::Error("Entity {} not found", id);
template <typename... Args> void Error(LocFmtStr<std::type_identity_t<Args>...> fmtLoc, Args &&...args)
{
    Logger &logger = GetLogger();
    if (!logger.IsEnabled(LogLevel::Error))
    {
        return;
    }
    logger.Log(LogLevel::Error, fmtLoc.loc, std::format(fmtLoc.fmt, std::forward<Args>(args)...));
}

/// @brief Logs a fatal error with automatic file/line capture.
///
/// Example: Log::Fatal("Unrecoverable state: {}", reason);
template <typename... Args> void Fatal(LocFmtStr<std::type_identity_t<Args>...> fmtLoc, Args &&...args)
{
    Logger &logger = GetLogger();
    if (!logger.IsEnabled(LogLevel::Fatal))
    {
        return;
    }
    logger.Log(LogLevel::Fatal, fmtLoc.loc, std::format(fmtLoc.fmt, std::forward<Args>(args)...));
}

} // namespace Log
} // namespace Assisi::Core
