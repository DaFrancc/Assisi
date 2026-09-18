/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file Chiara.cpp
/// @brief The capture runtime. Compiles to nothing unless ASSISI_ENABLE_CHIARA
///        is on — see the CMakeLists.

#if defined(ASSISI_CHIARA_ENABLED)

#include <Assisi/Chiara/Chiara.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
#include <cpuid.h>
#endif

namespace Assisi::Chiara
{
namespace Detail
{

std::atomic<bool> g_recording{false};
bool g_useHardwareTicks = false;

thread_local ThreadBuffer *t_buffer = nullptr;

} // namespace Detail

namespace
{

// --- Process-wide state -----------------------------------------------------
//
// Everything here is deliberately never destroyed. Threads can emit during
// static destruction (Jolt's pool, the NVML worker), and a capture runtime that
// tears itself down while its producers are still alive trades a profiling
// feature for a shutdown crash.

std::atomic<Detail::ThreadBuffer *> g_threadListHead{nullptr};
std::atomic<std::uint32_t> g_threadCount{0};
std::atomic<std::uint64_t> g_frameIndex{0};
std::atomic<std::uint64_t> g_nextFlowId{1};
std::atomic<std::uint64_t> g_nextAsyncId{1};

std::mutex g_initMutex;
bool g_initialized = false;
Config g_config = {};
double g_ticksPerSecond = 1'000'000'000.0;

// Main-thread only: when the last ~1 Hz clock snapshot went out.
std::uint64_t g_lastSnapshotTicks = 0;

std::mutex &InternMutex()
{
    static std::mutex *mutex = new std::mutex(); // NOLINT(cppcoreguidelines-owning-memory) — never freed, by design
    return *mutex;
}

std::unordered_set<std::string> &InternTable()
{
    static auto *table = new std::unordered_set<std::string>(); // NOLINT(cppcoreguidelines-owning-memory)
    return *table;
}

// --- Platform ---------------------------------------------------------------

[[nodiscard]] std::uint64_t CurrentOsThreadId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

void SetOsThreadName(const char *name) noexcept
{
#if defined(_WIN32)
    const int32_t wideLength = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (wideLength <= 0)
    {
        return;
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), wideLength);
    (void)::SetThreadDescription(::GetCurrentThread(), wide.c_str());
#elif defined(__linux__)
    // pthread caps the name at 16 bytes including the terminator.
    char truncated[16];
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
    (void)::pthread_setname_np(::pthread_self(), truncated);
#else
    (void)name;
#endif
}

/// @brief Whether the raw hardware counter can be trusted for a trace.
///
/// Two checks, not one. The invariant-TSC CPUID bit promises a *constant rate*,
/// which is not the same as being synchronized across sockets or surviving a
/// migrating VM — the kernel validates that separately and silently demotes the
/// clocksource when it fails. So we also ask the kernel what it settled on. When
/// either check fails we fall back to CLOCK_MONOTONIC_RAW, which costs a vDSO
/// call per event but is always right.
[[nodiscard]] bool HardwareClockIsTrustworthy()
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    bool invariant = false;
#if defined(_MSC_VER)
    int registers[4] = {0, 0, 0, 0};
    __cpuid(registers, static_cast<int>(0x80000000));
    if (static_cast<unsigned int>(registers[0]) >= 0x80000007u)
    {
        __cpuid(registers, static_cast<int>(0x80000007));
        invariant = (static_cast<unsigned int>(registers[3]) & (1u << 8u)) != 0u;
    }
#else
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid_max(0x80000000u, nullptr) >= 0x80000007u && __get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) != 0)
    {
        invariant = (edx & (1u << 8u)) != 0u;
    }
#endif
    if (!invariant)
    {
        return false;
    }
#elif defined(__aarch64__)
    // CNTVCT_EL0 is architecturally a fixed-frequency system counter; its
    // frequency register can lie, which is why we calibrate rather than read it.
#else
    return false;
#endif

#if defined(__linux__)
    // The kernel's verdict overrides ours: if it demoted the clocksource (bad
    // TSC sync, a VM that cannot guarantee it), so do we.
    std::ifstream source("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    if (source.is_open())
    {
        std::string selected;
        source >> selected;
        return selected == "tsc" || selected == "arch_sys_counter";
    }
#endif
    return true;
}

[[nodiscard]] std::uint64_t ReadHardwareTicks() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
#if defined(_MSC_VER)
    return __rdtsc();
#else
    return __builtin_ia32_rdtsc();
#endif
#elif defined(_M_X64) || defined(_M_IX86)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t counter = 0;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (counter));
    return counter;
#else
    return Detail::ReadFallbackTicks();
#endif
}

/// @brief Measures the hardware counter against the reference clock.
///
/// Calibrated on every architecture rather than read from a register: x86's
/// CPUID leaf 0x15 is absent or wrong often enough to be useless, and ARM's
/// CNTFRQ_EL0 can report a frequency the counter does not actually run at. The
/// ~1 Hz clock snapshots in the capture are the same measurement over a much
/// longer baseline, so a serializer can refine this after the fact for free.
[[nodiscard]] double CalibrateTicksPerSecond()
{
    constexpr auto kWindow = std::chrono::milliseconds(2);

    const std::uint64_t startNs = Detail::ReadFallbackTicks();
    const std::uint64_t startTicks = ReadHardwareTicks();
    std::this_thread::sleep_for(kWindow);
    const std::uint64_t endTicks = ReadHardwareTicks();
    const std::uint64_t endNs = Detail::ReadFallbackTicks();

    const std::uint64_t elapsedNs = endNs - startNs;
    const std::uint64_t elapsedTicks = endTicks - startTicks;
    if (elapsedNs == 0 || elapsedTicks == 0)
    {
        return 1'000'000'000.0;
    }
    return static_cast<double>(elapsedTicks) * 1'000'000'000.0 / static_cast<double>(elapsedNs);
}

[[nodiscard]] std::uint64_t CapacityFromBytes(std::uint32_t bytes)
{
    std::uint64_t events = bytes / sizeof(Event);
    events = std::max<std::uint64_t>(events, 64u);
    return std::bit_floor(events);
}

} // namespace

namespace Detail
{

std::uint64_t ReadFallbackTicks() noexcept
{
#if defined(_WIN32)
    static const std::uint64_t frequency = []
                                           {
                                               LARGE_INTEGER value{};
                                               ::QueryPerformanceFrequency(&value);
                                               return static_cast<std::uint64_t>(value.QuadPart);
                                           }();
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    const std::uint64_t ticks = static_cast<std::uint64_t>(counter.QuadPart);
    // Split to keep the nanosecond scaling from overflowing on a long uptime.
    return (ticks / frequency) * 1'000'000'000ull + ((ticks % frequency) * 1'000'000'000ull) / frequency;
#elif defined(__linux__)
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(now.tv_nsec);
#else
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since).count());
#endif
}

bool ThreadBuffer::TrySnapshotOpenScopes(std::vector<OpenScope> &out) const
{
    const std::uint32_t before = shadowGeneration.load(std::memory_order_acquire);
    if ((before & 1u) != 0u)
    {
        return false; // A push or pop is in flight.
    }

    const std::uint32_t rawDepth = shadowDepth.load(std::memory_order_acquire);
    const std::uint32_t depth = std::min(rawDepth, kMaxShadowDepth);

    out.clear();
    out.reserve(depth);
    for (std::uint32_t index = 0; index < depth; ++index)
    {
        OpenScope scope;
        scope.name = shadowNames[index].load(std::memory_order_acquire);
        scope.beginTicks = shadowBegins[index].load(std::memory_order_acquire);
        out.push_back(scope);
    }

    // Acquire on the entries above is what keeps this load from being hoisted
    // over them — the job a closing acquire fence would do, in a form the
    // sanitizer can actually see.
    return shadowGeneration.load(std::memory_order_acquire) == before;
}

/// @brief Allocates a buffer, names it, and publishes it to the reader's list.
///
/// Shared by the two things that need one: a thread registering itself, and a
/// track, which is a buffer no thread owns. Everything that differs between them
/// is decided by the caller — a track passes @p ownedByThread false, so it takes
/// no OS thread id and can never be mistaken for the main thread.
ThreadBuffer *CreateBuffer(const char *name, bool ownedByThread) noexcept
{
    {
        const std::lock_guard<std::mutex> lock(g_initMutex);
        if (!g_initialized)
        {
            return nullptr; // Pre-Initialize emission is a no-op, not an implicit start.
        }
    }

    const std::uint32_t index = g_threadCount.fetch_add(1, std::memory_order_relaxed);
    // Index zero is the thread that called Initialize. A track cannot be it —
    // Initialize registers the main thread before anything can ask for a track —
    // but saying so here means a reordering cannot quietly make a GPU row sort
    // itself to the top as though it were the main thread.
    const bool isMain = ownedByThread && index == 0;

    auto *buffer = new (std::nothrow) ThreadBuffer(); // NOLINT(cppcoreguidelines-owning-memory) — never freed
    if (buffer == nullptr)
    {
        return nullptr;
    }

    const std::uint64_t capacity =
        CapacityFromBytes(isMain ? g_config.mainThreadBufferBytes : g_config.otherThreadBufferBytes);
    buffer->storage = new (std::nothrow) Event[capacity];
    if (buffer->storage == nullptr)
    {
        return nullptr;
    }
    buffer->ring.Reset(buffer->storage, capacity);

    buffer->osThreadId = ownedByThread ? CurrentOsThreadId() : 0;
    buffer->registrationIndex = index;
    buffer->isMain = isMain;
    buffer->name.store(name != nullptr ? name : InternString("thread-" + std::to_string(index)),
                       std::memory_order_relaxed);

    // Publish last: `next` and every field above must be visible to a reader
    // that acquires the head.
    buffer->next = g_threadListHead.load(std::memory_order_acquire);
    while (!g_threadListHead.compare_exchange_weak(buffer->next, buffer, std::memory_order_release,
                                                   std::memory_order_acquire))
    {
    }
    return buffer;
}

ThreadBuffer *RegisterThreadBuffer(const char *name) noexcept
{
    if (t_buffer != nullptr)
    {
        if (name != nullptr)
        {
            t_buffer->name.store(name, std::memory_order_release);
        }
        return t_buffer;
    }

    ThreadBuffer *buffer = CreateBuffer(name, /*ownedByThread=*/ true);
    if (buffer == nullptr)
    {
        return nullptr;
    }
    t_buffer = buffer;
    return buffer;
}

} // namespace Detail

// --- Lifetime ---------------------------------------------------------------

void Initialize(const Config &config)
{
    {
        const std::lock_guard<std::mutex> lock(g_initMutex);
        if (g_initialized)
        {
            Detail::g_recording.store(true, std::memory_order_relaxed);
            return;
        }

        g_config = config;
        Detail::g_useHardwareTicks = HardwareClockIsTrustworthy();
        g_ticksPerSecond = Detail::g_useHardwareTicks ? CalibrateTicksPerSecond() : 1'000'000'000.0;
        g_initialized = true;
    }

    // Claim index 0 *before* recording opens. Registration only needs
    // g_initialized, so doing it the other way round would leave a window in
    // which another thread's first emit auto-registers as index 0 and gets the
    // 32 MiB main ring — with the actual main thread then sized as a worker.
    RegisterCurrentThread("main");
    Detail::g_recording.store(true, std::memory_order_release);

    g_lastSnapshotTicks = ReadTicks();
    EmitClockSnapshot();
}

void Shutdown()
{
    Detail::g_recording.store(false, std::memory_order_release);
}

void SetRecording(bool enabled)
{
    const std::lock_guard<std::mutex> lock(g_initMutex);
    if (!g_initialized)
    {
        return;
    }
    Detail::g_recording.store(enabled, std::memory_order_release);
}

// --- Threads ----------------------------------------------------------------

void RegisterCurrentThread(const char *name)
{
    const char *interned = InternString(name != nullptr ? std::string_view(name) : std::string_view("thread"));

    Detail::ThreadBuffer *buffer = Detail::RegisterThreadBuffer(interned);
    if (buffer == nullptr)
    {
        return; // Not initialized: nothing to name.
    }

    // Naming the main thread renames the process itself in top/ps, which is a
    // surprising side effect for a profiler to have.
    if (!buffer->isMain)
    {
        SetOsThreadName(interned);
    }
}

// --- Time and frames --------------------------------------------------------

double TicksPerSecond()
{
    return g_ticksPerSecond;
}

std::uint64_t MarkFrame()
{
    const std::uint64_t frame = g_frameIndex.fetch_add(1, std::memory_order_relaxed) + 1u;
    Detail::EmitRecord(EventType::FrameMark, "frame", frame);

    if (IsRecording())
    {
        const std::uint64_t now = ReadTicks();
        if (static_cast<double>(now - g_lastSnapshotTicks) >= g_ticksPerSecond)
        {
            g_lastSnapshotTicks = now;
            EmitClockSnapshot();
        }
    }
    return frame;
}

std::uint64_t CurrentFrame()
{
    return g_frameIndex.load(std::memory_order_relaxed);
}

void EmitClockSnapshot()
{
    Detail::EmitRecord(EventType::ClockSnapshot, "clock-snapshot", Detail::ReadFallbackTicks());
}

// --- Strings ----------------------------------------------------------------

const char *InternString(std::string_view text)
{
    const std::lock_guard<std::mutex> lock(InternMutex());
    // Node-based, so the stored string never moves and c_str() stays valid for
    // the life of the process even as the table rehashes.
    return InternTable().emplace(text).first->c_str();
}

// --- Emit -------------------------------------------------------------------

std::uint64_t NewFlowId()
{
    return g_nextFlowId.fetch_add(1, std::memory_order_relaxed);
}

void EmitArgString(const char *key, std::string_view value)
{
    if (!IsRecording())
    {
        return; // Skip the intern lock entirely when nothing is being captured.
    }
    EmitArgStringInterned(key, InternString(value));
}

std::uint64_t BeginAsync(const char *name)
{
    const std::uint64_t asyncId = g_nextAsyncId.fetch_add(1, std::memory_order_relaxed);
    Detail::EmitRecord(EventType::AsyncBegin, name, asyncId);
    return asyncId;
}

void EndAsync(const char *name, std::uint64_t asyncId)
{
    Detail::EmitRecord(EventType::AsyncEnd, name, asyncId);
}

// --- Tracks -----------------------------------------------------------------

Track *RegisterTrack(const char *name)
{
    return Detail::CreateBuffer(name, /*ownedByThread=*/ false);
}

std::uint64_t TrackLayout::GapTicks() noexcept
{
    // A trace rounds a slice's begin and its duration to the nanosecond
    // separately, so a reported end can miss the true one by half a nanosecond
    // each way, and the next slice's reported begin by another half. Two
    // nanoseconds clears all three.
    constexpr double kGapSeconds = 2e-9;
    const auto ticks = static_cast<std::uint64_t>(TicksPerSecond() * kGapSeconds);
    // A clock too coarse for two nanoseconds still has to separate two slices,
    // and one tick is the smallest distance it can express.
    return std::max<std::uint64_t>(ticks, 1);
}

void EmitScopeOn(Track *track, const char *name, std::uint64_t beginTicks, std::uint64_t durationTicks) noexcept
{
    if (track == nullptr || !IsRecording())
    {
        return;
    }
    // The one emit that does not read the clock. A Scope record is already
    // (begin, duration), so measuring elsewhere costs a push and no new format.
    Event record;
    record.timestampTicks = beginTicks;
    record.payload = durationTicks;
    record.name = name;
    record.type = EventType::Scope;
    track->ring.Push(record);
}

// --- Read side --------------------------------------------------------------

CaptureStats GetCaptureStats()
{
    CaptureStats stats;
    for (const Detail::ThreadBuffer *buffer = g_threadListHead.load(std::memory_order_acquire); buffer != nullptr;
         buffer = buffer->next)
    {
        const std::uint64_t cursor = buffer->ring.WriteCursor();
        stats.totalEventsWritten += cursor;
        stats.bufferWrapCount += buffer->ring.LostEvents(cursor);
        stats.threadCount += 1u;

        if (buffer->isMain && cursor > 0)
        {
            // Approximate while recording: the oldest record could be
            // overwritten mid-read if this thread were descheduled long enough
            // for the producer to lap an entire ring. A wrong number on a debug
            // readout, not a wrong capture — the serializer pauses first.
            const std::uint64_t begin = buffer->ring.ReadableBegin(cursor);
            const std::uint64_t oldest = buffer->ring.At(begin).timestampTicks;
            const std::uint64_t newest = buffer->ring.At(cursor - 1u).timestampTicks;
            if (newest > oldest)
            {
                stats.mainWindowSeconds = static_cast<double>(newest - oldest) / g_ticksPerSecond;
            }
        }
    }
    return stats;
}

std::vector<ThreadSnapshot> SnapshotThreads()
{
    std::vector<ThreadSnapshot> snapshots;
    for (const Detail::ThreadBuffer *buffer = g_threadListHead.load(std::memory_order_acquire); buffer != nullptr;
         buffer = buffer->next)
    {
        ThreadSnapshot snapshot;
        snapshot.name = buffer->name.load(std::memory_order_acquire);
        snapshot.osThreadId = buffer->osThreadId;
        snapshot.ring = &buffer->ring;
        snapshot.isMain = buffer->isMain;

        const std::uint64_t cursor = buffer->ring.WriteCursor();
        snapshot.beginIndex = buffer->ring.ReadableBegin(cursor);
        snapshot.endIndex = cursor;
        snapshot.lostEvents = buffer->ring.LostEvents(cursor);

        // Bounded retries: a thread churning scopes fast enough to lose eight
        // races in a row gets no open-scope synthesis rather than a torn one.
        constexpr std::int32_t kMaxAttempts = 8;
        for (std::int32_t attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            if (buffer->TrySnapshotOpenScopes(snapshot.openScopes))
            {
                break;
            }
            snapshot.openScopes.clear();
        }

        snapshots.push_back(std::move(snapshot));
    }
    return snapshots;
}

} // namespace Assisi::Chiara

#endif // ASSISI_CHIARA_ENABLED
