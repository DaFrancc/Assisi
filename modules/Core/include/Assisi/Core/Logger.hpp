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
/// @note Thread-safe. A single mutex serializes the entire Log() fan-out, so
/// concurrent Log() calls from any thread (e.g. Jolt's JobSystemThreadPool
/// workers firing a physics callback) neither race on the sink list nor
/// interleave partial lines on a shared sink (`std::cout`, the log file).
/// AddSink() takes the same lock, so reconfiguring at runtime is also safe;
/// SetMinLevel() needs no lock because the level is atomic. The lock is held
/// across formatting and writing — fine at the engine's current log volume;
/// revisit only if logging shows up in a profile.
///
/// @note Fatal is the one exception: it try-locks and writes regardless, so a
/// crash handler cannot hang on a lock held by the thread that just died. A
/// Fatal racing another logging thread may interleave its line. See the
/// comment on AcquireForWrite in Logger.cpp.
struct Logger
{
    /// @brief Adds an output sink. Multiple sinks can be active simultaneously.
    void AddSink(std::shared_ptr<Sink> sink);

    /// @brief Sets the minimum level — messages below this level are discarded.
    void SetMinLevel(LogLevel level);

    /// @brief Returns whether a message at this level would be emitted.
    ///
    /// The Log:: free functions call this *before* formatting, so a suppressed
    /// message costs one relaxed atomic load rather than a full std::format and
    /// its string allocation. Relaxed is the right ordering: a level change
    /// racing a log call may be observed by either, and both outcomes are
    /// correct — there is no other state the level publishes.
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

/// @note Every function here checks the level *before* formatting. Formatting a
/// message that the level filter then drops is pure waste — it runs the
/// formatter for every argument and allocates a string only to discard it — and
/// it is waste the caller cannot avoid, since the arguments are already
/// evaluated by the time we are called. The early-out is what makes raising the
/// minimum level actually cheapen the build rather than merely quieten it.
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
