/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ChiaraTest.hpp
/// @brief Shared setup for the capture tests.
///
/// Chiara's state is process-wide and deliberately never torn down (buffers
/// outlive every thread, by design), so the suite initializes it once and
/// isolates cases by *cursor range* instead: mark the main ring's cursor, do the
/// work, read back only what landed after the mark. That gives each case a clean
/// view without a reset entry point existing purely for tests.

#include <Assisi/Chiara/Chiara.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Assisi::ChiaraTest
{

#if defined(ASSISI_CHIARA_ENABLED)

/// @brief Starts the capture once per process, with rings small enough to be
/// cheap and large enough that no test wraps them.
inline void EnsureInitialized()
{
    static const bool started = []
                                {
                                    Chiara::Config config;
                                    config.mainThreadBufferBytes  = 1u << 20;// 32768 records
                                    config.otherThreadBufferBytes = 1u << 16; // 2048 records
                                    Chiara::Initialize(config);
                                    return true;
                                }();
    (void)started;
}

/// @brief The main thread's snapshot, or a default one if it has not registered.
[[nodiscard]] inline Chiara::ThreadSnapshot MainSnapshot()
{
    for (Chiara::ThreadSnapshot &snapshot : Chiara::SnapshotThreads())
    {
        if (snapshot.isMain)
        {
            return snapshot;
        }
    }
    return {};
}

/// @brief Where the main ring's cursor is right now — the mark a case takes
/// before emitting.
[[nodiscard]] inline std::uint64_t MainCursor()
{
    return MainSnapshot().endIndex;
}

/// @brief Every record the main thread emitted at or after `mark`.
[[nodiscard]] inline std::vector<Chiara::Event> MainEventsSince(std::uint64_t mark)
{
    const Chiara::ThreadSnapshot snapshot = MainSnapshot();
    std::vector<Chiara::Event>   events;
    if (snapshot.ring == nullptr)
    {
        return events;
    }
    for (std::uint64_t index = std::max(mark, snapshot.beginIndex); index < snapshot.endIndex; ++index)
    {
        events.push_back(snapshot.ring->At(index));
    }
    return events;
}

/// @brief Filters a record list down to one kind.
[[nodiscard]] inline std::vector<Chiara::Event> OfType(const std::vector<Chiara::Event> &events, Chiara::EventType type)
{
    std::vector<Chiara::Event> matching;
    for (const Chiara::Event &event : events)
    {
        if (event.type == type)
        {
            matching.push_back(event);
        }
    }
    return matching;
}

#endif // ASSISI_CHIARA_ENABLED

} // namespace Assisi::ChiaraTest
