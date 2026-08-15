/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Serializer.cpp
/// @brief Chrome JSON export. Compiles to nothing unless ASSISI_ENABLE_CHIARA is on.
///
/// The JSON emission at the bottom is the easy half. The real work is rebuilding
/// which scope each arg belongs to: a scope is only written when it *ends*, so
/// its args reached the ring first, and the ownership that was implicit in the
/// call stack has to be recovered from timestamps.

#if defined(ASSISI_CHIARA_ENABLED)

#    include <Assisi/Chiara/Serializer.hpp>

#    include <algorithm>
#    include <bit>
#    include <chrono>
#    include <cstdio>
#    include <format>
#    include <fstream>
#    include <map>
#    include <memory>
#    include <mutex>
#    include <string_view>
#    include <thread>
#    include <vector>

namespace Assisi::Chiara
{
namespace
{

constexpr std::int32_t kProcessId = 1;

/// @brief A slice being assembled: a finished scope, or one still running that
/// was recovered from a shadow stack.
struct Slice
{
    std::uint64_t beginTicks = 0;
    std::uint64_t endTicks   = 0;
    const char *name       = nullptr;
    bool stillOpen  = false;

    // Args are folded into the owning slice's `args` object rather than emitted
    // as their own trace events — that is what makes "click a slice, see which
    // asset" work in the viewer with no tooling of ours.
    std::vector<std::pair<const char *, const char *>> stringArgs;
    std::vector<std::pair<const char *, std::uint64_t>> intArgs;
};

/// @brief A point record waiting to be bound to whichever slice encloses it.
struct PendingArg
{
    std::uint64_t ticks    = 0;
    const char *key      = nullptr;
    const char *text     = nullptr;   // Set for ArgString.
    std::uint64_t value    = 0;       // Set for ArgU64.
    bool isString = false;
};

/// @brief Everything one thread contributes to the trace.
struct ThreadTrace
{
    const char *name     = nullptr;
    std::int32_t traceTid = 0;
    bool isMain   = false;
    std::vector<Slice> slices;
    std::vector<Event> pointEvents; // Counters, flows, async, frame marks, instants.
};

[[nodiscard]] std::string EscapeJson(const char *text)
{
    std::string escaped;
    if (text == nullptr)
    {
        return escaped;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor)
    {
        const unsigned char character = static_cast<unsigned char>(*cursor);
        switch (character)
        {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (character < 0x20u)
            {
                escaped += std::format("\\u{:04x}", static_cast<std::uint32_t>(character));
            }
            else
            {
                escaped += static_cast<char>(character);
            }
            break;
        }
    }
    return escaped;
}

/// @brief Buffered text sink. The trace is streamed rather than built as one
/// string: a full ring is 130-150 MB, which is fine on disk and not fine as a
/// single allocation.
class TraceWriter
{
public:
    explicit TraceWriter(const std::filesystem::path &path) : _out(path, std::ios::binary | std::ios::trunc)
    {
        _buffer.reserve(kFlushThreshold + 4096u);
    }

    [[nodiscard]] bool IsOpen() const { return _out.is_open(); }

    template <typename... Args> void Write(std::format_string<Args...> pattern, Args &&... args)
    {
        std::format_to(std::back_inserter(_buffer), pattern, std::forward<Args>(args)...);
        if (_buffer.size() >= kFlushThreshold)
        {
            Flush();
        }
    }

    void Flush()
    {
        if (!_buffer.empty())
        {
            _out.write(_buffer.data(), static_cast<std::streamsize>(_buffer.size()));
            _bytesWritten += _buffer.size();
            _buffer.clear();
        }
    }

    [[nodiscard]] std::uint64_t BytesWritten() const { return _bytesWritten; }

private:
    static constexpr std::size_t kFlushThreshold = 1u << 20;

    std::ofstream _out;
    std::string _buffer;
    std::uint64_t _bytesWritten = 0;
};

/// @brief Converts a raw tick value to microseconds since the trace origin.
[[nodiscard]] double ToMicroseconds(std::uint64_t ticks, std::uint64_t originTicks, double ticksPerSecond)
{
    const double elapsed = static_cast<double>(ticks - originTicks);
    return elapsed * 1'000'000.0 / ticksPerSecond;
}

/// @brief Assigns each arg to the innermost slice containing it.
///
/// Scopes on one thread are stack-disciplined, so sorting slices by start (outer
/// first on a tie) and sweeping the args in time order lets a simple stack track
/// what is open: the back of the stack is always the innermost live slice.
/// Anything left unclaimed is an arg emitted outside any scope — dropped, but
/// counted, because a silently vanishing arg looks exactly like one that was
/// never emitted.
[[nodiscard]] std::uint64_t BindArgsToSlices(std::vector<Slice> &slices, std::vector<PendingArg> &args,
                                             std::vector<PendingArg> *unbound = nullptr)
{
    std::ranges::sort(slices,
                      [](const Slice &left, const Slice &right)
            {
                if (left.beginTicks != right.beginTicks)
                {
                    return left.beginTicks < right.beginTicks;
                }
                return left.endTicks > right.endTicks;           // Enclosing slice first.
            });
    std::ranges::sort(args, [](const PendingArg &left, const PendingArg &right) { return left.ticks < right.ticks; });

    std::uint64_t orphaned = 0;
    std::vector<std::size_t>  openStack;
    std::size_t nextSlice = 0;

    for (const PendingArg &arg : args)
    {
        while (nextSlice < slices.size() && slices[nextSlice].beginTicks <= arg.ticks)
        {
            openStack.push_back(nextSlice);
            ++nextSlice;
        }
        while (!openStack.empty() && slices[openStack.back()].endTicks < arg.ticks)
        {
            openStack.pop_back();
        }

        if (openStack.empty())
        {
            ++orphaned;
            if (unbound != nullptr)
            {
                unbound->push_back(arg);
            }
            continue;
        }

        Slice &owner = slices[openStack.back()];
        if (arg.isString)
        {
            owner.stringArgs.emplace_back(arg.key, arg.text);
        }
        else
        {
            owner.intArgs.emplace_back(arg.key, arg.value);
        }
    }
    return orphaned;
}

/// @brief Recomputes the tick rate from the capture's own clock snapshots.
///
/// The value measured at Initialize comes from a 2 ms window; two snapshots
/// seconds apart measure the same thing over a baseline thousands of times
/// longer. This is why the snapshots are emitted at all, and it costs nothing.
[[nodiscard]] double RefineTicksPerSecond(const std::vector<std::pair<std::uint64_t, std::uint64_t>> &snapshots,
                                          double fallback)
{
    if (snapshots.size() < 2)
    {
        return fallback;
    }
    const auto &[firstTicks, firstNanos] = snapshots.front();
    const auto &[lastTicks, lastNanos]   = snapshots.back();
    if (lastNanos <= firstNanos || lastTicks <= firstTicks)
    {
        return fallback;
    }

    const double elapsedNanos = static_cast<double>(lastNanos - firstNanos);
    if (elapsedNanos < 500'000'000.0) // Under half a second the short window is no worse.
    {
        return fallback;
    }
    return static_cast<double>(lastTicks - firstTicks) * 1'000'000'000.0 / elapsedNanos;
}

void WriteSlice(TraceWriter &writer, const Slice &slice, std::int32_t traceTid, std::uint64_t originTicks,
                double ticksPerSecond, bool &needsComma)
{
    const double begin    = ToMicroseconds(slice.beginTicks, originTicks, ticksPerSecond);
    const double duration = ToMicroseconds(slice.endTicks, originTicks, ticksPerSecond) - begin;

    writer.Write("{}\n{{\"ph\":\"X\",\"pid\":{},\"tid\":{},\"name\":\"{}\",\"ts\":{:.3f},\"dur\":{:.3f}",
                 needsComma ? "," : "", kProcessId, traceTid, EscapeJson(slice.name), begin, duration);
    needsComma = true;

    const bool hasArgs = !slice.stringArgs.empty() || !slice.intArgs.empty() || slice.stillOpen;
    if (!hasArgs)
    {
        writer.Write("}}");
        return;
    }

    writer.Write(",\"args\":{{");
    bool firstArg = true;
    for (const auto &[key, text] : slice.stringArgs)
    {
        writer.Write("{}\"{}\":\"{}\"", firstArg ? "" : ",", EscapeJson(key), EscapeJson(text));
        firstArg = false;
    }
    for (const auto &[key, value] : slice.intArgs)
    {
        writer.Write("{}\"{}\":{}", firstArg ? "" : ",", EscapeJson(key), value);
        firstArg = false;
    }
    if (slice.stillOpen)
    {
        // Flagged rather than silently extended to the window edge: a slice that
        // never ended and one that ended exactly at the dump look identical
        // otherwise, and the difference is the whole point of the hang case.
        writer.Write("{}\"chiara.still_open\":true", firstArg ? "" : ",");
    }
    writer.Write("}}}}");
}

void WritePointEvent(TraceWriter &writer, const Event &event, std::int32_t traceTid, std::uint64_t originTicks,
                     double ticksPerSecond, bool &needsComma)
{
    const double timestamp = ToMicroseconds(event.timestampTicks, originTicks, ticksPerSecond);
    const auto comma     = needsComma ? "," : "";

    switch (event.type)
    {
    case EventType::Counter:
        // `group/name` so the viewer groups related tracks together.
        writer.Write("{}\n{{\"ph\":\"C\",\"pid\":{},\"tid\":{},\"name\":\"{}\",\"ts\":{:.3f},\"args\":{{\"v\":{}}}}}",
                     comma, kProcessId, traceTid, EscapeJson(event.name), timestamp,
                     std::bit_cast<double>(event.payload));
        break;

    case EventType::FlowBegin:
        writer.Write("{}\n{{\"ph\":\"s\",\"pid\":{},\"tid\":{},\"cat\":\"flow\",\"name\":\"{}\",\"id\":{},\"ts\":{:."
                     "3f}}}",
                     comma, kProcessId, traceTid, EscapeJson(event.name), event.payload, timestamp);
        break;

    case EventType::FlowEnd:
        // bp:"e" binds the arrow to the enclosing slice rather than the next one
        // to start, which is what makes it land on the work that paid the cost.
        writer.Write("{}\n{{\"ph\":\"f\",\"pid\":{},\"tid\":{},\"cat\":\"flow\",\"name\":\"{}\",\"id\":{},\"bp\":\"e\","
                     "\"ts\":{:.3f}}}",
                     comma, kProcessId, traceTid, EscapeJson(event.name), event.payload, timestamp);
        break;

    case EventType::AsyncBegin:
        // Chrome JSON pairs async events by (cat, id), so `cat` is load-bearing
        // here rather than decoration — without it nothing pairs.
        writer.Write("{}\n{{\"ph\":\"b\",\"pid\":{},\"tid\":{},\"cat\":\"async\",\"name\":\"{}\",\"id\":{},\"ts\":{:."
                     "3f}}}",
                     comma, kProcessId, traceTid, EscapeJson(event.name), event.payload, timestamp);
        break;

    case EventType::AsyncEnd:
        writer.Write("{}\n{{\"ph\":\"e\",\"pid\":{},\"tid\":{},\"cat\":\"async\",\"name\":\"{}\",\"id\":{},\"ts\":{:."
                     "3f}}}",
                     comma, kProcessId, traceTid, EscapeJson(event.name), event.payload, timestamp);
        break;

    case EventType::FrameMark:
        // An instant to see, plus a counter to plot and to query frame-over-frame.
        writer.Write("{}\n{{\"ph\":\"i\",\"pid\":{},\"tid\":{},\"name\":\"frame\",\"ts\":{:.3f},\"s\":\"g\"}},"
                     "\n{{\"ph\":\"C\",\"pid\":{},\"tid\":{},\"name\":\"frame\",\"ts\":{:.3f},\"args\":{{\"frame\":{}}}}}",
                     comma, kProcessId, traceTid, timestamp, kProcessId, traceTid, timestamp, event.payload);
        break;

    case EventType::Instant:
        writer.Write("{}\n{{\"ph\":\"i\",\"pid\":{},\"tid\":{},\"name\":\"{}\",\"ts\":{:.3f},\"s\":\"t\"}}", comma,
                     kProcessId, traceTid, EscapeJson(event.name), timestamp);
        break;

    default:
        return; // Scopes, args and clock snapshots are handled elsewhere.
    }
    needsComma = true;
}

/// @brief Pulls `[from, to)` out of one thread's ring into the three shapes the
/// writer needs. Shared by the one-shot dump and the session drain so they can
/// never disagree about what a record means.
void GatherRange(const ThreadSnapshot &snapshot, std::uint64_t from, std::uint64_t to, std::uint64_t windowBegin,
                 std::vector<Slice> &slices, std::vector<PendingArg> &args, std::vector<Event> &pointEvents)
{
    for (std::uint64_t index = from; index < to; ++index)
    {
        const Event &event = snapshot.ring->At(index);

        if (event.type == EventType::Scope)
        {
            // Kept if it *overlaps* the window rather than starts inside it: a
            // long scope straddling the edge is exactly the one worth seeing,
            // and clipping it away would orphan its args.
            const std::uint64_t endTicks = event.timestampTicks + event.payload;
            if (endTicks < windowBegin)
            {
                continue;
            }
            Slice slice;
            slice.beginTicks = event.timestampTicks;
            slice.endTicks   = endTicks;
            slice.name       = event.name;
            slices.push_back(std::move(slice));
            continue;
        }

        if (event.timestampTicks < windowBegin)
        {
            continue;
        }

        switch (event.type)
        {
        case EventType::ArgString:
        {
            PendingArg arg;
            arg.ticks    = event.timestampTicks;
            arg.key      = event.name;
            arg.text     = std::bit_cast<const char *>(event.payload);
            arg.isString = true;
            args.push_back(arg);
            break;
        }
        case EventType::ArgU64:
        {
            PendingArg arg;
            arg.ticks = event.timestampTicks;
            arg.key   = event.name;
            arg.value = event.payload;
            args.push_back(arg);
            break;
        }
        case EventType::ClockSnapshot:
            break; // Calibration, not something to look at.
        default:
            pointEvents.push_back(event);
            break;
        }
    }
}

// --- Session state ----------------------------------------------------------

/// Ceiling on args held across chunks for one thread. A scope open for the whole
/// session would otherwise accumulate its args forever; past this many, waiting
/// has clearly stopped working and they are counted orphaned instead.
constexpr std::size_t kMaxCarriedArgs = 4096;

/// One thread's place in an ongoing session.
struct SessionThread
{
    std::int32_t traceTid        = 0;
    std::uint64_t nextIndex       = 0;
    bool metadataWritten = false;
    /// Args whose enclosing scope had not ended yet when the last chunk was
    /// written. A scope reaches the ring only when it closes, so an arg near a
    /// chunk boundary routinely outlives its owner's absence — dropping it here
    /// would silently lose exactly the context on long-running work.
    std::vector<PendingArg> carriedArgs;
};

struct Session
{
    std::mutex mutex;
    bool active = false;
    std::unique_ptr<TraceWriter>                        writer;
    std::filesystem::path path;
    std::uint64_t originTicks    = 0;
    double ticksPerSecond = 1.0;
    bool needsComma     = false;
    std::int32_t nextTraceTid   = 0;
    std::uint64_t eventsWritten  = 0;
    std::uint64_t orphanedArgs   = 0;
    std::uint64_t drains         = 0;
    std::uint64_t eventsLost     = 0;
    std::uint64_t lastDrainTicks = 0;
    std::uint64_t lastTotalEvents = 0;
    std::map<const Detail::EventRing *, SessionThread>  threads;
};

Session &TheSession()
{
    static Session session;
    return session;
}

/// @brief Writes everything the rings hold since the last drain. Caller holds
/// the session mutex and has already stopped the producers.
void DrainLocked(Session &session)
{
    const std::vector<ThreadSnapshot> snapshots = SnapshotThreads();

    for (const ThreadSnapshot &snapshot : snapshots)
    {
        if (snapshot.ring == nullptr)
        {
            continue;
        }

        auto [entry, inserted] = session.threads.try_emplace(snapshot.ring);
        SessionThread &thread  = entry->second;
        if (inserted)
        {
            thread.traceTid = session.nextTraceTid++;
            // A thread that appears mid-session starts where it is now, not at
            // the beginning of its ring — anything older predates the session.
            thread.nextIndex = snapshot.beginIndex;
        }

        if (!thread.metadataWritten)
        {
            session.writer->Write(
                "{}\n{{\"ph\":\"M\",\"pid\":{},\"tid\":{},\"name\":\"thread_name\",\"args\":{{\"name\":\"{}\"}}}}",
                session.needsComma ? "," : "", kProcessId, thread.traceTid, EscapeJson(snapshot.name));
            session.writer->Write(",\n{{\"ph\":\"M\",\"pid\":{},\"tid\":{},\"name\":\"thread_sort_index\",\"args\":{{"
                                  "\"sort_index\":{}}}}}",
                                  kProcessId, thread.traceTid, snapshot.isMain ? 0 : thread.traceTid + 1);
            session.needsComma      = true;
            thread.metadataWritten  = true;
        }

        // The ring dropped records we had not read yet: the drain did not keep
        // up. Counted rather than papered over — a trace with a hole in it that
        // does not say so is worse than one that admits it.
        if (snapshot.beginIndex > thread.nextIndex)
        {
            session.eventsLost += snapshot.beginIndex - thread.nextIndex;
            thread.nextIndex = snapshot.beginIndex;
        }

        std::vector<Slice>      slices;
        std::vector<PendingArg> args = std::move(thread.carriedArgs);
        std::vector<Event>      pointEvents;
        thread.carriedArgs.clear();

        GatherRange(snapshot, thread.nextIndex, snapshot.endIndex, /*windowBegin=*/ 0, slices, args, pointEvents);
        thread.nextIndex = snapshot.endIndex;

        std::vector<PendingArg> unbound;
        (void)BindArgsToSlices(slices, args, &unbound);

        // An arg that found no owner in this chunk is not necessarily orphaned:
        // its scope may simply still be running, in which case it will close in
        // a later chunk and claim it then. The shadow stack is what tells the
        // two cases apart — if the arg was emitted at or after the outermost
        // still-open scope began, an open scope can still be its owner. Carrying
        // by "newer than any slice in this chunk" instead silently drops every
        // arg followed by shorter sibling work, which is the common case.
        std::uint64_t oldestOpenBegin = UINT64_MAX;
        for (const OpenScope &open : snapshot.openScopes)
        {
            oldestOpenBegin = std::min(oldestOpenBegin, open.beginTicks);
        }

        for (const PendingArg &arg : unbound)
        {
            if (arg.ticks >= oldestOpenBegin && thread.carriedArgs.size() < kMaxCarriedArgs)
            {
                thread.carriedArgs.push_back(arg);
            }
            else
            {
                // Genuinely ownerless — emitted outside any scope — or the carry
                // list is saturated by a scope that has stayed open so long it is
                // not going to be resolved by waiting.
                ++session.orphanedArgs;
            }
        }

        for (const Slice &slice : slices)
        {
            WriteSlice(*session.writer, slice, thread.traceTid, session.originTicks, session.ticksPerSecond,
                       session.needsComma);
            ++session.eventsWritten;
        }
        for (const Event &event : pointEvents)
        {
            WritePointEvent(*session.writer, event, thread.traceTid, session.originTicks, session.ticksPerSecond,
                            session.needsComma);
            ++session.eventsWritten;
        }
    }

    session.writer->Flush();
    ++session.drains;
    session.lastDrainTicks  = ReadTicks();
    session.lastTotalEvents = GetCaptureStats().totalEventsWritten;
}

} // namespace

bool BeginSession(const std::filesystem::path &path)
{
    Session &session = TheSession();
    const std::lock_guard<std::mutex> lock(session.mutex);
    if (session.active)
    {
        return false;
    }

    session.writer = std::make_unique<TraceWriter>(path);
    if (!session.writer->IsOpen())
    {
        session.writer.reset();
        return false;
    }

    session.path            = path;
    session.originTicks     = ReadTicks();
    session.ticksPerSecond  = TicksPerSecond();
    session.needsComma      = false;
    session.nextTraceTid    = 0;
    session.eventsWritten   = 0;
    session.orphanedArgs    = 0;
    session.drains          = 0;
    session.eventsLost      = 0;
    session.lastDrainTicks  = session.originTicks;
    session.lastTotalEvents = GetCaptureStats().totalEventsWritten;
    session.threads.clear();

    session.writer->Write("{{\"displayTimeUnit\":\"ms\",\"traceEvents\":[");
    session.writer->Write("\n{{\"ph\":\"M\",\"pid\":{},\"tid\":0,\"name\":\"process_name\",\"args\":{{\"name\":"
                          "\"Assisi\"}}}}",
                          kProcessId);
    session.needsComma = true;
    session.writer->Flush();

    // Everything already in the rings predates the session; the first drain
    // starts from wherever each thread is now.
    for (const ThreadSnapshot &snapshot : SnapshotThreads())
    {
        if (snapshot.ring == nullptr)
        {
            continue;
        }
        SessionThread &thread = session.threads[snapshot.ring];
        thread.traceTid       = session.nextTraceTid++;
        thread.nextIndex      = snapshot.endIndex;
    }

    session.active = true;
    return true;
}

void PumpSession()
{
    Session &session = TheSession();
    if (!session.active)
    {
        return; // The overwhelmingly common case: a few loads and out.
    }

    const std::lock_guard<std::mutex> lock(session.mutex);
    if (!session.active)
    {
        return;
    }

    // Drain on whichever comes first: enough events that the smallest ring could
    // start dropping, or enough time that a quiet period still reaches disk.
    // Both are needed — the first alone would never flush an idle session, and
    // the second alone would lose a burst.
    constexpr std::uint64_t kEventsPerDrain  = 20'000;
    constexpr double kSecondsPerDrain = 2.0;

    const std::uint64_t totalEvents = GetCaptureStats().totalEventsWritten;
    const std::uint64_t sinceDrain  = totalEvents - session.lastTotalEvents;
    const double elapsed =
        static_cast<double>(ReadTicks() - session.lastDrainTicks) / session.ticksPerSecond;

    if (sinceDrain < kEventsPerDrain && elapsed < kSecondsPerDrain)
    {
        return;
    }

    // Same protocol as a one-shot dump: stop the producers, let the pause become
    // visible, read, resume. The pause is what makes reading the rings safe at
    // all, and it is why draining happens on a schedule rather than constantly.
    const bool wasRecording = IsRecording();
    SetRecording(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    DrainLocked(session);
    SetRecording(wasRecording);
}

SerializeResult EndSession()
{
    Session &session = TheSession();
    const std::lock_guard<std::mutex> lock(session.mutex);

    SerializeResult result;
    if (!session.active)
    {
        result.error = "no session is running";
        return result;
    }

    const bool wasRecording = IsRecording();
    SetRecording(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    DrainLocked(session);
    SetRecording(wasRecording);

    session.writer->Write("\n]}}\n");
    session.writer->Flush();

    result.success        = true;
    result.eventsWritten  = session.eventsWritten;
    result.orphanedArgs   = session.orphanedArgs;
    result.threadsWritten = session.threads.size();
    result.bytesWritten   = session.writer->BytesWritten();
    result.ticksPerSecond = session.ticksPerSecond;
    result.windowSeconds  = static_cast<double>(ReadTicks() - session.originTicks) / session.ticksPerSecond;

    session.writer.reset();
    session.active = false;
    return result;
}

SessionStats GetSessionStats()
{
    Session &session = TheSession();
    const std::lock_guard<std::mutex> lock(session.mutex);

    SessionStats stats;
    stats.active = session.active;
    if (!session.active)
    {
        return stats;
    }
    stats.elapsedSeconds = static_cast<double>(ReadTicks() - session.originTicks) / session.ticksPerSecond;
    stats.eventsWritten  = session.eventsWritten;
    stats.bytesWritten   = session.writer != nullptr ? session.writer->BytesWritten() : 0;
    stats.drains         = session.drains;
    stats.eventsLost     = session.eventsLost;
    stats.path           = session.path.string();
    return stats;
}

SerializeResult SerializeCapture(const std::filesystem::path &path, double lastSeconds)
{
    SerializeResult result;

    const bool wasRecording = IsRecording();
    SetRecording(false);

    // Producers stop at their next recording check, so one push may still be in
    // flight per thread — which the ring's one-slot sacrifice already covers.
    // The pause needs a moment to become visible, though: a thread that has not
    // yet seen it could otherwise push its way past a reader entirely. This is
    // the drain, and it is why a dump is a background job.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    const std::vector<ThreadSnapshot> snapshots = SnapshotThreads();
    const std::uint64_t dumpTicks = ReadTicks();

    // --- Pass one: bounds and clock -----------------------------------------
    std::uint64_t newestTicks = 0;
    std::uint64_t oldestTicks = UINT64_MAX;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> clockSnapshots;

    for (const ThreadSnapshot &snapshot : snapshots)
    {
        if (snapshot.ring == nullptr)
        {
            continue;
        }
        for (std::uint64_t index = snapshot.beginIndex; index < snapshot.endIndex; ++index)
        {
            const Event &event = snapshot.ring->At(index);
            ++result.eventsRead;

            // A scope's payload is its duration, so its end can be newer than
            // any point record; every other type ends where it starts.
            const std::uint64_t endTicks =
                event.type == EventType::Scope ? event.timestampTicks + event.payload : event.timestampTicks;
            newestTicks = std::max(newestTicks, endTicks);
            oldestTicks = std::min(oldestTicks, event.timestampTicks);
            if (event.type == EventType::ClockSnapshot)
            {
                clockSnapshots.emplace_back(event.timestampTicks, event.payload);
            }
        }
    }

    if (result.eventsRead == 0)
    {
        SetRecording(wasRecording);
        result.error = "nothing captured";
        return result;
    }

    std::ranges::sort(clockSnapshots);
    const double ticksPerSecond = RefineTicksPerSecond(clockSnapshots, TicksPerSecond());
    result.ticksPerSecond       = ticksPerSecond;

    newestTicks = std::max(newestTicks, dumpTicks);

    std::uint64_t windowBegin = oldestTicks;
    if (lastSeconds > 0.0)
    {
        const auto windowTicks = static_cast<std::uint64_t>(lastSeconds * ticksPerSecond);
        windowBegin            = windowTicks >= newestTicks ? 0 : newestTicks - windowTicks;
        windowBegin            = std::max(windowBegin, oldestTicks);
    }
    result.windowSeconds = static_cast<double>(newestTicks - windowBegin) / ticksPerSecond;

    // --- Pass two: gather, per thread ---------------------------------------
    std::vector<ThreadTrace> traces;
    traces.reserve(snapshots.size());

    for (const ThreadSnapshot &snapshot : snapshots)
    {
        if (snapshot.ring == nullptr)
        {
            continue;
        }

        ThreadTrace trace;
        trace.name     = snapshot.name;
        trace.isMain   = snapshot.isMain;
        trace.traceTid = static_cast<std::int32_t>(traces.size());

        std::vector<PendingArg> args;
        GatherRange(snapshot, snapshot.beginIndex, snapshot.endIndex, windowBegin, trace.slices, args,
                    trace.pointEvents);

        // Scopes that never ended have no record at all, so they are recovered
        // from the thread's shadow stack and drawn as running until the dump.
        for (const OpenScope &open : snapshot.openScopes)
        {
            if (open.name == nullptr || open.beginTicks == 0)
            {
                continue;
            }
            Slice slice;
            slice.beginTicks = open.beginTicks;
            slice.endTicks   = std::max(open.beginTicks, dumpTicks);
            slice.name       = open.name;
            slice.stillOpen  = true;
            trace.slices.push_back(std::move(slice));
            ++result.slicesSynthesized;
        }

        result.orphanedArgs += BindArgsToSlices(trace.slices, args);
        traces.push_back(std::move(trace));
    }

    // Recording can resume now: everything still needed lives in `traces`, and
    // the file write below never touches a ring again.
    SetRecording(wasRecording);

    // --- Write ---------------------------------------------------------------
    TraceWriter writer(path);
    if (!writer.IsOpen())
    {
        result.error = "could not open " + path.string() + " for writing";
        return result;
    }

    writer.Write("{{\"displayTimeUnit\":\"ms\",\"traceEvents\":[");
    bool needsComma = false;

    writer.Write("\n{{\"ph\":\"M\",\"pid\":{},\"tid\":0,\"name\":\"process_name\",\"args\":{{\"name\":\"Assisi\"}}}}",
                 kProcessId);
    needsComma = true;

    for (const ThreadTrace &trace : traces)
    {
        writer.Write(",\n{{\"ph\":\"M\",\"pid\":{},\"tid\":{},\"name\":\"thread_name\",\"args\":{{\"name\":\"{}\"}}}}",
                     kProcessId, trace.traceTid, EscapeJson(trace.name));
        // Main first, everyone else after — the frame timeline is what you look
        // at, and hunting for it among worker lanes is a papercut every time.
        writer.Write(",\n{{\"ph\":\"M\",\"pid\":{},\"tid\":{},\"name\":\"thread_sort_index\",\"args\":{{\"sort_index\":"
                     "{}}}}}",
                     kProcessId, trace.traceTid, trace.isMain ? 0 : trace.traceTid + 1);
    }

    for (const ThreadTrace &trace : traces)
    {
        for (const Slice &slice : trace.slices)
        {
            WriteSlice(writer, slice, trace.traceTid, windowBegin, ticksPerSecond, needsComma);
            ++result.eventsWritten;
        }
        for (const Event &event : trace.pointEvents)
        {
            WritePointEvent(writer, event, trace.traceTid, windowBegin, ticksPerSecond, needsComma);
            ++result.eventsWritten;
        }
    }

    writer.Write("\n]}}\n");
    writer.Flush();

    result.threadsWritten = traces.size();
    result.bytesWritten   = writer.BytesWritten();
    result.success        = true;
    return result;
}

} // namespace Assisi::Chiara

#endif // ASSISI_CHIARA_ENABLED
