/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestLogger.cpp
/// @brief Level parsing and the minimum-level filter.
///
/// These are the contract behind `--verbosity`: a name either resolves to a
/// level or is refused, and a level below the minimum produces nothing.

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/Logger.hpp>

using namespace Assisi::Core;

namespace
{

/// The logger is a process-wide singleton, so a test that changes the level has
/// to put it back or it leaks into every test that runs after it.
struct LevelGuard
{
    LevelGuard() = default;
    // Back to the build's default, not a hardcoded Trace — a shipping build
    // starts at Info and this would otherwise leave it more verbose than it was.
    ~LevelGuard() { GetLogger().SetMinLevel(DefaultMinLevel()); }
    LevelGuard(const LevelGuard &)            = delete;
    LevelGuard &operator=(const LevelGuard &) = delete;
};

struct CapturingSink final : Sink
{
    std::vector<std::string> messages;
    void Write(LogLevel, std::string_view message) override { messages.emplace_back(message); }

    [[nodiscard]] bool Mentions(std::string_view needle) const
    {
        for (const std::string &message : messages)
        {
            if (message.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE("The default level follows the build configuration")
{
    // Developer builds keep everything; a shipped log starts at the narrative
    // and leaves out instrumentation aimed at whoever wrote the code.
#ifdef ASSISI_SHIPPING_BUILD
    CHECK(DefaultMinLevel() == LogLevel::Info);
    CHECK_FALSE(GetLogger().IsEnabled(LogLevel::Debug));
#else
    CHECK(DefaultMinLevel() == LogLevel::Trace);
    CHECK(GetLogger().IsEnabled(LogLevel::Trace));
#endif
    CHECK(GetLogger().IsEnabled(LogLevel::Info));
}

TEST_CASE("ParseLogLevel accepts every level name, in any case")
{
    CHECK(ParseLogLevel("trace") == LogLevel::Trace);
    CHECK(ParseLogLevel("debug") == LogLevel::Debug);
    CHECK(ParseLogLevel("info") == LogLevel::Info);
    CHECK(ParseLogLevel("warn") == LogLevel::Warn);
    CHECK(ParseLogLevel("error") == LogLevel::Error);
    CHECK(ParseLogLevel("fatal") == LogLevel::Fatal);

    // --verbosity Info and --verbosity INFO are the same request.
    CHECK(ParseLogLevel("INFO") == LogLevel::Info);
    CHECK(ParseLogLevel("Warn") == LogLevel::Warn);

    // Round-trips through the canonical name.
    for (const std::string_view name : LogLevelNames())
    {
        const std::optional<LogLevel> parsed = ParseLogLevel(name);
        REQUIRE(parsed.has_value());
        CHECK(LogLevelName(*parsed) == name);
    }
}

TEST_CASE("ParseLogLevel refuses anything that is not a name")
{
    // Ordinals specifically. The enumerators are dense, so inserting a level
    // renumbers the rest and a "--verbosity 2" in someone's shortcut would
    // silently come to mean a different level. Refusing is the whole point.
    CHECK_FALSE(ParseLogLevel("0").has_value());
    CHECK_FALSE(ParseLogLevel("2").has_value());
    CHECK_FALSE(ParseLogLevel("5").has_value());

    CHECK_FALSE(ParseLogLevel("").has_value());
    CHECK_FALSE(ParseLogLevel("chatty").has_value());
    CHECK_FALSE(ParseLogLevel("inf").has_value());   // prefix of a real name
    CHECK_FALSE(ParseLogLevel("information").has_value()); // real name as prefix
}

TEST_CASE("SetMinLevel suppresses everything below it")
{
    const LevelGuard guard;
    auto sink = std::make_shared<CapturingSink>();
    GetLogger().AddSink(sink);

    GetLogger().SetMinLevel(LogLevel::Warn);
    Log::Trace("trace-marker");
    Log::Debug("debug-marker");
    Log::Info("info-marker");
    Log::Warn("warn-marker");
    Log::Error("error-marker");

    CHECK_FALSE(sink->Mentions("trace-marker"));
    CHECK_FALSE(sink->Mentions("debug-marker"));
    CHECK_FALSE(sink->Mentions("info-marker"));
    CHECK(sink->Mentions("warn-marker"));
    CHECK(sink->Mentions("error-marker"));
}

TEST_CASE("IsEnabled agrees with the level that was set")
{
    const LevelGuard guard;

    GetLogger().SetMinLevel(LogLevel::Error);
    CHECK_FALSE(GetLogger().IsEnabled(LogLevel::Trace));
    CHECK_FALSE(GetLogger().IsEnabled(LogLevel::Info));
    CHECK_FALSE(GetLogger().IsEnabled(LogLevel::Warn));
    CHECK(GetLogger().IsEnabled(LogLevel::Error));
    CHECK(GetLogger().IsEnabled(LogLevel::Fatal));

    // The guard for an expensive call site is this same predicate, so it has to
    // agree with what the free functions do rather than approximate it.
    GetLogger().SetMinLevel(LogLevel::Trace);
    for (const std::string_view name : LogLevelNames())
    {
        CHECK(GetLogger().IsEnabled(*ParseLogLevel(name)));
    }
}
