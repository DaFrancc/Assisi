/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Event.hpp
/// @brief The 32-byte capture record and the single-producer ring that holds it
///        (design: docs/chiara-design-notes.md §4).
///
/// Kept free of any runtime state so the collector, the serializer and the tests
/// all agree on one definition, and so a ring can be built standalone in a test
/// without touching the process-wide capture.
///
/// The record is deliberately fixed-size: per-event overwrite is only safe
/// because there is no variable-length framing to tear, which is what lets the
/// oldest event be discarded without a chunk-granularity scheme.

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace Assisi::Chiara
{

/// @brief What a record means. The payload's interpretation depends entirely on
/// this, so the two are read together and never separately.
enum class EventType : std::uint8_t
{
    Scope = 0,         ///< timestampTicks = begin; payload = duration in ticks.
    Counter = 1,       ///< payload = the double, bit_cast to u64.
    FlowBegin = 2,     ///< payload = flow id (the cause).
    FlowEnd = 3,       ///< payload = flow id (the effect).
    FrameMark = 4,     ///< payload = frame index.
    ArgString = 5,     ///< payload = interned value pointer; binds to the enclosing scope.
    ArgU64 = 6,        ///< payload = the value; binds to the enclosing scope.
    AsyncBegin = 7,    ///< payload = async id; NOT stack-disciplined.
    AsyncEnd = 8,      ///< payload = async id.
    ClockSnapshot = 9, ///< payload = CLOCK_MONOTONIC_RAW nanoseconds at timestampTicks.
    Instant = 10,      ///< Reserved: no macro emits one yet (see §4 of the design notes).
};

/// @brief One capture record. Trivially copyable and exactly 32 bytes, so a
/// push is a pair of stores and a ring slot can never be half-formatted.
///
/// `name` is an *interned* pointer, never owned: macro sites pass string
/// literals (program lifetime) and dynamic names go through InternString once.
/// It is dereferenced only when serializing, never while capturing.
struct Event
{
    std::uint64_t timestampTicks = 0; ///< Raw hardware ticks; converted to time at dump.
    std::uint64_t payload        = 0; ///< Meaning depends on `type`.
    const char *name           = nullptr;   ///< Interned; not owned.
    EventType type           = EventType::Instant;
    std::uint8_t reserved0      = 0;
    std::uint16_t reserved1      = 0;
    std::uint32_t reserved2      = 0;
};

static_assert(sizeof(Event) == 32, "Event must stay 32 bytes — ring sizing and the wrap math assume it");
static_assert(alignof(Event) == 8, "Event must stay 8-byte aligned so a slot store is never split");
static_assert(std::is_trivially_copyable_v<Event>, "Ring slots are overwritten in place");

/// @brief A scope that was still open when the capture was read — recovered from
/// the shadow stack rather than the ring, since a scope only reaches the ring
/// when it ends (§4: the hang case).
struct OpenScope
{
    const char *name       = nullptr;
    std::uint64_t beginTicks = 0;
};

/// @brief Deepest scope nesting the shadow stack records per thread. Beyond it
/// the depth still counts (so pushes and pops stay balanced) but the entries are
/// not retained — 64 is far past any real call graph we scope.
inline constexpr std::uint32_t kMaxShadowDepth = 64;

namespace Detail
{

/// @brief Single-producer ring of fixed-size records, overwriting the oldest
/// when full.
///
/// The owning thread is the only writer; any thread may read. Correctness rests
/// on two things:
///
/// 1. The cursor is published with release *after* the slot is written, and read
///    with acquire, so a reader that observes cursor C has a happens-before edge
///    to every slot below C.
/// 2. The reader takes `[C - capacity + 1, C)`, **not** `[C - capacity, C)`.
///    A producer caught mid-push writes slot `C & mask`, which aliases index
///    `C - capacity` — the oldest live record. Reading it would race a partial
///    write. Sacrificing that one slot removes the only unsynchronized read.
///
/// Point 2 costs one event per ring and is invisible in any other framing; it is
/// wrong only on the full-ring "dump everything" path, which is exactly the path
/// that matters. Producers must be stopped (recording paused) before a read, so
/// each can complete at most one straggler past the observed cursor.
class EventRing
{
public:
    EventRing()                             = default;
    EventRing(const EventRing &)            = delete;
    EventRing &operator=(const EventRing &) = delete;
    EventRing(EventRing &&)                 = delete;
    EventRing &operator=(EventRing &&)      = delete;
    ~EventRing()                            = default;

    /// @brief Points the ring at caller-owned storage. `capacity` must be a
    /// power of two; storage must hold that many records and outlive the ring.
    void Reset(Event *storage, std::uint64_t capacity) noexcept
    {
        _storage  = storage;
        _capacity = capacity;
        _mask     = capacity - 1u;
        _writeCursor.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t Capacity() const noexcept { return _capacity; }

    /// @brief Appends a record. Producer thread only.
    void Push(const Event &event) noexcept
    {
        // Only this thread writes the cursor, so a relaxed load of it is exact.
        const std::uint64_t index = _writeCursor.load(std::memory_order_relaxed);
        _storage[index & _mask]   = event;
        _writeCursor.store(index + 1u, std::memory_order_release);
    }

    /// @brief Total records ever pushed. Acquire: pairs with Push's release, so
    /// every slot below the returned value is safe to read.
    [[nodiscard]] std::uint64_t WriteCursor() const noexcept { return _writeCursor.load(std::memory_order_acquire); }

    /// @brief First index a reader may touch given an observed cursor — see the
    /// class comment for why this is not simply `cursor - capacity`.
    [[nodiscard]] std::uint64_t ReadableBegin(std::uint64_t cursor) const noexcept
    {
        if (cursor >= _capacity)
        {
            return cursor - _capacity + 1u;
        }
        return 0;
    }

    /// @brief Records dropped to overwrite. Reported so a too-small ring is
    /// visible rather than silently truncating history.
    [[nodiscard]] std::uint64_t LostEvents(std::uint64_t cursor) const noexcept
    {
        return cursor > _capacity ? cursor - _capacity : 0;
    }

    /// @brief The record at a monotonic index. Only valid for indices inside
    /// `[ReadableBegin(cursor), cursor)` for a cursor obtained from WriteCursor.
    [[nodiscard]] const Event &At(std::uint64_t index) const noexcept { return _storage[index & _mask]; }

private:
    Event *_storage  = nullptr;
    std::uint64_t _capacity = 0;
    std::uint64_t _mask     = 0;
    std::atomic<std::uint64_t> _writeCursor{0};
};

} // namespace Detail

} // namespace Assisi::Chiara
