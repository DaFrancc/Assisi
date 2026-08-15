/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Chiara.hpp
/// @brief The capture runtime — rings, threads, clocks, and the emit entry
///        points (design: docs/chiara-design-notes.md).
///
/// Chiara records what the engine spent time and memory on, into per-thread ring
/// buffers, and hands a recent window to a serializer on demand. It is a capture
/// pipeline only: there is no viewer here and no third-party profiler linked in.
///
/// Most call sites want Profile.hpp (the macros) rather than this header. This
/// one is for the glue: starting the runtime, naming threads, pumping counters,
/// and reading the rings back out.
///
/// **Compiled out unless asked.** `ASSISI_ENABLE_CHIARA` defaults OFF on every
/// configuration; the `-c` make targets (`gd-c`, `gs-c`, …) turn it on. When it
/// is off every entry point below is an inline no-op, so glue code never needs
/// an `#ifdef`, and the .cpp compiles to nothing.
///
/// **Before Initialize, every entry point is a safe no-op.** That is
/// load-bearing rather than defensive: Core's JobSystem runs in headless tests
/// with no Application and therefore no InitGuard, and a Chiara-enabled build of
/// those tests must behave exactly like a default one.
///
/// Threading: emitting is lock-free on every path but one — EmitArgString with a
/// dynamic value must intern it, and interning takes a mutex. That is acceptable
/// precisely because arg-carrying events are rare by design (asset publishes,
/// not per-entity work); a hot site that cares interns once itself and calls
/// EmitArgStringInterned.

#include <Assisi/Chiara/Event.hpp>

#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#    include <intrin.h>
#endif

namespace Assisi::Chiara
{

/// @brief Ring sizes, fixed at Initialize. Defaults hold roughly 20 s of a
/// heavy 144 fps frame on the main thread and hours on a worker.
struct Config
{
    std::uint32_t mainThreadBufferBytes  = 32u << 20;
    std::uint32_t otherThreadBufferBytes = 2u << 20;
};

/// @brief What the capture has held since Initialize. Surfaced in the panel so a
/// too-small ring is visible rather than silently dropping the interesting part.
struct CaptureStats
{
    std::uint64_t totalEventsWritten = 0;
    std::uint64_t bufferWrapCount    = 0;   ///< Records lost to overwrite, all rings.
    std::uint32_t threadCount        = 0;
    double mainWindowSeconds  = 0.0;        ///< Time span the main ring currently covers.
};

/// @brief One thread's readable window, taken under a paused capture. The index
/// range is already narrowed to what is safe to read (Event.hpp, EventRing).
struct ThreadSnapshot
{
    const char *name       = nullptr;
    std::uint64_t osThreadId = 0;
    const Detail::EventRing *ring      = nullptr;
    std::uint64_t beginIndex = 0;           ///< Inclusive.
    std::uint64_t endIndex   = 0;           ///< Exclusive.
    std::uint64_t lostEvents = 0;
    bool isMain     = false;
    std::vector<OpenScope>  openScopes;     ///< Still open at snapshot time (§4, the hang case).
};

#if defined(ASSISI_CHIARA_ENABLED)

namespace Detail
{

/// @brief True only between Initialize and Shutdown *and* while not paused. One
/// relaxed load of this is the entire cost of an emit that does not record, and
/// it starts false, which is what makes pre-Initialize calls safe.
extern std::atomic<bool> g_recording;

/// @brief Whether the hardware counter passed its gate at Initialize. When it
/// did not, ticks come from CLOCK_MONOTONIC_RAW instead and are nanoseconds.
extern bool g_useHardwareTicks;

[[nodiscard]] std::uint64_t ReadFallbackTicks() noexcept;

/// @brief Per-thread capture state. Allocated on first emit, never freed —
/// which makes late-thread teardown (Jolt's pool, the NVML worker, static
/// destructors) a non-issue rather than a lifetime puzzle.
struct ThreadBuffer
{
    EventRing ring;
    Event *storage = nullptr;

    // The shadow stack of currently-open scopes. A scope only reaches the ring
    // when it ends, so without this a capture taken during a hang shows nothing
    // for the very scope that is hanging.
    //
    // The serializer reads it while this thread keeps pushing and popping, so
    // it is a seqlock: the generation is odd while the stack is being mutated,
    // and a reader that sees an odd or changed generation retries. Entries are
    // atomics rather than plain fields so that concurrent read is well-defined
    // instead of a race that merely happens to work.
    //
    // Ordering rides on the entries' own acquire/release rather than the two
    // standalone fences a textbook seqlock uses: same guarantees, but GCC's
    // ThreadSanitizer does not model atomic_thread_fence at all, so a fenced
    // version is invisible to the one tool that can check it.
    std::atomic<std::uint32_t> shadowGeneration{0};
    std::atomic<std::uint32_t> shadowDepth{0};
    std::atomic<const char *>  shadowNames[kMaxShadowDepth]{};
    std::atomic<std::uint64_t> shadowBegins[kMaxShadowDepth]{};

    // Atomic because a thread may name itself *after* it has already emitted
    // and been published — JobSystem workers register late — and the serializer
    // could be reading the list at that moment.
    std::atomic<const char *> name{nullptr};

    std::uint64_t osThreadId        = 0;
    std::uint32_t registrationIndex = 0;
    bool isMain            = false;
    ThreadBuffer *next              = nullptr;

    /// @brief Records a scope as open. Owner thread only.
    void PushOpenScope(const char *scopeName, std::uint64_t beginTicks) noexcept
    {
        const std::uint32_t generation = shadowGeneration.load(std::memory_order_relaxed);
        shadowGeneration.store(generation + 1u, std::memory_order_relaxed);

        const std::uint32_t depth = shadowDepth.load(std::memory_order_relaxed);
        if (depth < kMaxShadowDepth)
        {
            shadowNames[depth].store(scopeName, std::memory_order_release);
            shadowBegins[depth].store(beginTicks, std::memory_order_release);
        }
        // Counted even past the retained depth, so pushes and pops stay balanced.
        shadowDepth.store(depth + 1u, std::memory_order_release);

        shadowGeneration.store(generation + 2u, std::memory_order_release);
    }

    /// @brief Drops the innermost open scope. Owner thread only, and called even
    /// when the scope's record is not emitted — an unbalanced stack is worse
    /// than a missing slice.
    void PopOpenScope() noexcept
    {
        const std::uint32_t generation = shadowGeneration.load(std::memory_order_relaxed);
        shadowGeneration.store(generation + 1u, std::memory_order_relaxed);

        const std::uint32_t depth = shadowDepth.load(std::memory_order_relaxed);
        if (depth > 0u)
        {
            shadowDepth.store(depth - 1u, std::memory_order_release);
        }

        shadowGeneration.store(generation + 2u, std::memory_order_release);
    }

    /// @brief Copies the open scopes out. Returns false if the stack changed
    /// underneath — the caller retries.
    [[nodiscard]] bool TrySnapshotOpenScopes(std::vector<OpenScope> &out) const;
};

extern thread_local ThreadBuffer *t_buffer;

/// @brief Allocates and registers a buffer for the calling thread. Slow path,
/// taken once per thread; returns nullptr before Initialize.
ThreadBuffer *RegisterThreadBuffer(const char *name) noexcept;

[[nodiscard]] inline ThreadBuffer *AcquireThreadBuffer() noexcept
{
    ThreadBuffer *buffer = t_buffer;
    if (buffer == nullptr) [[unlikely]]
    {
        buffer = RegisterThreadBuffer(nullptr);
    }
    return buffer;
}

} // namespace Detail

/// @brief Reads the capture clock. Raw hardware ticks when the counter passed
/// its gate, CLOCK_MONOTONIC_RAW nanoseconds otherwise; TicksPerSecond() says
/// which, and the conversion to wall time happens once, at dump.
[[nodiscard]] inline std::uint64_t ReadTicks() noexcept
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (Detail::g_useHardwareTicks)
    {
#    if defined(_MSC_VER)
        return __rdtsc();
#    else
        return __builtin_ia32_rdtsc();
#    endif
    }
#elif defined(__aarch64__)
    if (Detail::g_useHardwareTicks)
    {
        std::uint64_t counter = 0;
        __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (counter));
        return counter;
    }
#endif
    return Detail::ReadFallbackTicks();
}

/// @brief Whether records are being written right now. One relaxed load.
[[nodiscard]] inline bool IsRecording() noexcept
{
    return Detail::g_recording.load(std::memory_order_relaxed);
}

namespace Detail
{

/// @brief The whole emit path: bail if not recording, find this thread's ring,
/// stamp, push. Everything public below is a one-liner over this.
inline void EmitRecord(EventType type, const char *name, std::uint64_t payload) noexcept
{
    if (!IsRecording())
    {
        return;
    }
    ThreadBuffer *buffer = AcquireThreadBuffer();
    if (buffer == nullptr) [[unlikely]]
    {
        return;
    }

    Event record;
    record.timestampTicks = ReadTicks();
    record.payload        = payload;
    record.name           = name;
    record.type           = type;
    buffer->ring.Push(record);
}

} // namespace Detail

// --- Lifetime ---------------------------------------------------------------

/// @brief Starts the capture: sizes the rings, gates the clock, registers the
/// caller as the main thread, and begins recording. Idempotent.
void Initialize(const Config &config = {});

/// @brief Stops recording. Never frees — see ThreadBuffer.
void Shutdown();

/// @brief Pauses or resumes recording. No effect before Initialize.
void SetRecording(bool enabled);

// --- Threads ----------------------------------------------------------------

/// @brief Names the calling thread for the capture and, except for the main
/// thread, for the OS as well (renaming the main thread would rename the whole
/// process in top/ps). Safe to call before Initialize; does nothing then.
void RegisterCurrentThread(const char *name);

// --- Time and frames --------------------------------------------------------

/// @brief Ticks per second for the clock in use — the divisor that turns stored
/// ticks into seconds at dump time.
[[nodiscard]] double TicksPerSecond();

/// @brief Advances the frame counter and marks the boundary. Main thread only.
/// Also emits the ~1 Hz clock snapshot that keeps ticks correlatable with
/// CLOCK_MONOTONIC_RAW.
std::uint64_t MarkFrame();

[[nodiscard]] std::uint64_t CurrentFrame();

/// @brief Pairs the current tick value with CLOCK_MONOTONIC_RAW. Emitted
/// automatically at Initialize and ~1 Hz from MarkFrame.
void EmitClockSnapshot();

// --- Strings ----------------------------------------------------------------

/// @brief Returns a pointer with program lifetime for `text`, deduplicated.
/// Takes a mutex: call it once and cache the result, never per frame. Works
/// before Initialize.
[[nodiscard]] const char *InternString(std::string_view text);

// --- Emit -------------------------------------------------------------------

/// @brief A fresh id for linking a cause to its later effect. Never returns 0.
[[nodiscard]] std::uint64_t NewFlowId();

inline void EmitCounter(const char *name, double value) noexcept
{
    Detail::EmitRecord(EventType::Counter, name, std::bit_cast<std::uint64_t>(value));
}

inline void EmitFlowBegin(const char *name, std::uint64_t flowId) noexcept
{
    Detail::EmitRecord(EventType::FlowBegin, name, flowId);
}

inline void EmitFlowEnd(const char *name, std::uint64_t flowId) noexcept
{
    Detail::EmitRecord(EventType::FlowEnd, name, flowId);
}

inline void EmitArgU64(const char *key, std::uint64_t value) noexcept
{
    Detail::EmitRecord(EventType::ArgU64, key, value);
}

/// @brief Attaches context to the innermost open scope. **Interns `value`, so
/// this is the one emit that takes a lock** — see the file comment.
void EmitArgString(const char *key, std::string_view value);

/// @brief EmitArgString for a caller that already interned the value.
inline void EmitArgStringInterned(const char *key, const char *value) noexcept
{
    Detail::EmitRecord(EventType::ArgString, key, std::bit_cast<std::uint64_t>(value));
}

/// @brief Opens a span for work that will finish on another thread — a job
/// continuation, a streaming load. Distinct from ScopeTimer because these are
/// not stack-disciplined. Returns the id to close it with.
[[nodiscard]] std::uint64_t BeginAsync(const char *name);

void EndAsync(const char *name, std::uint64_t asyncId);

// --- Read side --------------------------------------------------------------

[[nodiscard]] CaptureStats GetCaptureStats();

/// @brief Every registered thread's readable window plus its open scopes.
/// Call with recording paused: a running producer can lap the reader, which the
/// one-slot sacrifice does not protect against.
[[nodiscard]] std::vector<ThreadSnapshot> SnapshotThreads();

// --- RAII -------------------------------------------------------------------

/// @brief Times a scope and emits one record when it ends. Complete events
/// rather than a begin/end pair: half the pushes, and help-waiting nests
/// correctly for free (an inner task run inside Task::Wait is wall-clock
/// contained in the waiting scope, which is the truth).
class ScopeTimer
{
public:
    explicit ScopeTimer(const char *name) noexcept
    {
        if (!IsRecording())
        {
            return;
        }
        Detail::ThreadBuffer *buffer = Detail::AcquireThreadBuffer();
        if (buffer == nullptr) [[unlikely]]
        {
            return;
        }
        _buffer     = buffer;
        _name       = name;
        _beginTicks = ReadTicks();
        _buffer->PushOpenScope(name, _beginTicks);
    }

    ~ScopeTimer() noexcept
    {
        if (_buffer == nullptr)
        {
            return;
        }
        const std::uint64_t endTicks = ReadTicks();
        _buffer->PopOpenScope();

        // A dump may have paused recording since the constructor ran. Emitting
        // anyway would break the at-most-one-straggler bound the reader depends
        // on, so the record is dropped instead — the capture is blind while it
        // writes, by design. The pop above happened regardless.
        if (!IsRecording())
        {
            return;
        }

        Event record;
        record.timestampTicks = _beginTicks;
        record.payload        = endTicks - _beginTicks;
        record.name           = _name;
        record.type           = EventType::Scope;
        _buffer->ring.Push(record);
    }

    ScopeTimer(const ScopeTimer &)            = delete;
    ScopeTimer &operator=(const ScopeTimer &) = delete;
    ScopeTimer(ScopeTimer &&)                 = delete;
    ScopeTimer &operator=(ScopeTimer &&)      = delete;

private:
    Detail::ThreadBuffer *_buffer     = nullptr;
    const char *_name       = nullptr;
    std::uint64_t _beginTicks = 0;
};

/// @brief Initialize on construction, Shutdown on destruction. Declared as
/// Application's first member so the runtime is up before any worker spawns and
/// goes down last.
class InitGuard
{
public:
    explicit InitGuard(const Config &config = {}) { Initialize(config); }
    ~InitGuard() { Shutdown(); }

    InitGuard(const InitGuard &)            = delete;
    InitGuard &operator=(const InitGuard &) = delete;
    InitGuard(InitGuard &&)                 = delete;
    InitGuard &operator=(InitGuard &&)      = delete;
};

#else // !ASSISI_CHIARA_ENABLED

// Inline no-ops so glue code compiles unchanged in a default build. Nothing
// here emits code; the .cpp is excluded from the build entirely.

inline void Initialize(const Config & = {}) {}
inline void Shutdown() {}
inline void SetRecording(bool) {}
inline void RegisterCurrentThread(const char *) {}

[[nodiscard]] inline bool          IsRecording() noexcept { return false; }
[[nodiscard]] inline std::uint64_t ReadTicks() noexcept { return 0; }
[[nodiscard]] inline double        TicksPerSecond() { return 1.0; }

inline std::uint64_t               MarkFrame() { return 0; }
[[nodiscard]] inline std::uint64_t CurrentFrame() { return 0; }
inline void                        EmitClockSnapshot() {}

[[nodiscard]] inline const char *InternString(std::string_view) { return ""; }
[[nodiscard]] inline std::uint64_t NewFlowId() { return 1; }

inline void EmitCounter(const char *, double) noexcept {}
inline void EmitFlowBegin(const char *, std::uint64_t) noexcept {}
inline void EmitFlowEnd(const char *, std::uint64_t) noexcept {}
inline void EmitArgU64(const char *, std::uint64_t) noexcept {}
inline void EmitArgString(const char *, std::string_view) {}
inline void EmitArgStringInterned(const char *, const char *) noexcept {}

[[nodiscard]] inline std::uint64_t BeginAsync(const char *) { return 0; }
inline void                        EndAsync(const char *, std::uint64_t) {}

[[nodiscard]] inline CaptureStats                GetCaptureStats() { return {}; }
[[nodiscard]] inline std::vector<ThreadSnapshot> SnapshotThreads() { return {}; }

class ScopeTimer
{
public:
    explicit ScopeTimer(const char *) noexcept {}
    ~ScopeTimer() = default;

    ScopeTimer(const ScopeTimer &)            = delete;
    ScopeTimer &operator=(const ScopeTimer &) = delete;
    ScopeTimer(ScopeTimer &&)                 = delete;
    ScopeTimer &operator=(ScopeTimer &&)      = delete;
};

class InitGuard
{
public:
    explicit InitGuard(const Config & = {}) {}
    ~InitGuard() = default;

    InitGuard(const InitGuard &)            = delete;
    InitGuard &operator=(const InitGuard &) = delete;
    InitGuard(InitGuard &&)                 = delete;
    InitGuard &operator=(InitGuard &&)      = delete;
};

#endif // ASSISI_CHIARA_ENABLED

} // namespace Assisi::Chiara
