/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LogCapture.hpp
/// @brief Reads back what the engine logged, for the tests whose subject *is* a
///        log line.
///
/// Most behaviour is observable in the scene and should be asserted there. A
/// warning is different: "nulled with a warning" and "nulled silently" produce
/// identical worlds, and the whole point of the warning is that somebody finds
/// out. A test that cannot see it is the test round 7 found documenting the
/// absence of the thing it was named for.
///
/// The logger has AddSink and no RemoveSink, so the sink is installed once for
/// the process and cleared per use rather than attached and detached per test.

#include <Assisi/Core/Logger.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Tests
{

/// Records every line the logger emits, under its own lock: the logger fans out
/// to sinks from whichever thread logged, and one test in this binary logs from
/// several.
struct RecordingSink final : Core::Sink
{
    void Write(Core::LogLevel, std::string_view message) override
    {
        const std::lock_guard lock{_mutex};
        _messages.emplace_back(message);
    }

    void Clear()
    {
        const std::lock_guard lock{_mutex};
        _messages.clear();
    }

    [[nodiscard]] bool Mentions(std::string_view needle) const
    {
        const std::lock_guard lock{_mutex};
        for (const std::string &message : _messages)
        {
            if (message.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

  private:
    mutable std::mutex       _mutex;
    std::vector<std::string> _messages;
};

/// Installs the sink on first use and hands back the same one afterwards.
inline RecordingSink &InstalledSink()
{
    static const std::shared_ptr<RecordingSink> sink = []
    {
        auto created = std::make_shared<RecordingSink>();
        Core::GetLogger().AddSink(created);
        return created;
    }();
    return *sink;
}

/// Clears the log on construction, so a test reads only its own lines.
class LogCapture
{
  public:
    LogCapture() { InstalledSink().Clear(); }

    [[nodiscard]] bool Mentions(std::string_view needle) const { return InstalledSink().Mentions(needle); }
};

} // namespace Assisi::Tests
