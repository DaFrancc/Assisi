/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file TestEventRing.cpp
/// @brief The record layout and the ring's read protocol (design §4/§5).
///
/// These run in *both* build configurations — Event.hpp carries no runtime state
/// and is compiled either way — so the ring's arithmetic is covered even in a
/// default build where the rest of Chiara is gone.

#include <Assisi/Chiara/Event.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Assisi::Chiara;

namespace
{

/// @brief A record that can prove it was not stitched together from two
/// different writes: every field is derived from one seed.
[[nodiscard]] Event SelfCheckingEvent(std::uint64_t seed)
{
    Event event;
    event.timestampTicks = seed;
    event.payload        = seed * 0x9E3779B97F4A7C15ull;
    event.type           = EventType::Counter;
    event.reserved2      = static_cast<std::uint32_t>(seed);
    return event;
}

[[nodiscard]] bool IsIntact(const Event &event)
{
    return event.payload == event.timestampTicks * 0x9E3779B97F4A7C15ull
           && event.reserved2 == static_cast<std::uint32_t>(event.timestampTicks);
}

} // namespace

TEST_CASE("Event stays exactly 32 bytes")
{
    // Enforced at compile time in the header; asserted here too so the failure
    // names the reason rather than pointing at a header line.
    CHECK(sizeof(Event) == 32);
    CHECK(alignof(Event) == 8);
    CHECK(sizeof(EventType) == 1);
}

TEST_CASE("A fresh ring reads back everything pushed")
{
    constexpr std::uint64_t kCapacity = 16;
    std::vector<Event>      storage(kCapacity);
    Detail::EventRing       ring;
    ring.Reset(storage.data(), kCapacity);

    for (std::uint64_t index = 0; index < 10; ++index)
    {
        ring.Push(SelfCheckingEvent(index));
    }

    const std::uint64_t cursor = ring.WriteCursor();
    CHECK(cursor == 10);
    CHECK(ring.ReadableBegin(cursor) == 0); // Nothing has wrapped, so nothing is sacrificed.
    CHECK(ring.LostEvents(cursor) == 0);

    for (std::uint64_t index = 0; index < 10; ++index)
    {
        CHECK(ring.At(index).timestampTicks == index);
    }
}

TEST_CASE("A wrapped ring drops the oldest records and says so")
{
    constexpr std::uint64_t kCapacity = 8;
    std::vector<Event>      storage(kCapacity);
    Detail::EventRing       ring;
    ring.Reset(storage.data(), kCapacity);

    for (std::uint64_t index = 0; index < 20; ++index)
    {
        ring.Push(SelfCheckingEvent(index));
    }

    const std::uint64_t cursor = ring.WriteCursor();
    CHECK(cursor == 20);
    CHECK(ring.LostEvents(cursor) == 12);

    // The readable window is capacity-1, not capacity: one slot is given up so a
    // producer caught mid-push cannot be read.
    const std::uint64_t begin = ring.ReadableBegin(cursor);
    CHECK(begin == 13);
    CHECK(cursor - begin == kCapacity - 1);

    for (std::uint64_t index = begin; index < cursor; ++index)
    {
        CHECK(ring.At(index).timestampTicks == index);
    }
}

TEST_CASE("The sacrificed slot is exactly the one a straggler would write")
{
    // This is the invariant the whole read protocol rests on. A producer caught
    // mid-push writes slot `cursor & mask`; the naive window `[C-capacity, C)`
    // starts at an index that maps to that very slot, so reading it would race a
    // half-written record. The +1 is what removes the overlap.
    constexpr std::uint64_t kCapacity = 8;
    constexpr std::uint64_t kMask     = kCapacity - 1;
    std::vector<Event>      storage(kCapacity);
    Detail::EventRing       ring;
    ring.Reset(storage.data(), kCapacity);

    for (std::uint64_t index = 0; index < 32; ++index)
    {
        ring.Push(SelfCheckingEvent(index));
    }

    const std::uint64_t cursor = ring.WriteCursor();
    const std::uint64_t begin  = ring.ReadableBegin(cursor);

    CHECK((begin & kMask) != (cursor & kMask));        // Never reads the straggler's slot.
    CHECK(((cursor - kCapacity) & kMask) == (cursor & kMask)); // ...which the naive window would have.
}

TEST_CASE("A straggler push cannot disturb an already-observed window")
{
    // The single-threaded proof of the same property: take the window a reader
    // would take, then let the producer complete one more push — exactly what it
    // is allowed to do after recording is paused — and confirm nothing the
    // reader was going to read moved.
    constexpr std::uint64_t kCapacity = 8;
    std::vector<Event>      storage(kCapacity);
    Detail::EventRing       ring;
    ring.Reset(storage.data(), kCapacity);

    for (std::uint64_t index = 0; index < 20; ++index)
    {
        ring.Push(SelfCheckingEvent(index));
    }

    const std::uint64_t cursor = ring.WriteCursor();
    const std::uint64_t begin  = ring.ReadableBegin(cursor);

    std::vector<Event> before;
    for (std::uint64_t index = begin; index < cursor; ++index)
    {
        before.push_back(ring.At(index));
    }

    ring.Push(SelfCheckingEvent(999)); // The straggler.

    for (std::uint64_t index = begin; index < cursor; ++index)
    {
        CHECK(ring.At(index).timestampTicks == before[index - begin].timestampTicks);
        CHECK(IsIntact(ring.At(index)));
    }
}

TEST_CASE("Reading a ring while its producer is stopped never sees a torn record")
{
    // The threaded form of the contract: a producer runs flat out on a tiny ring
    // (so it wraps constantly), is asked to stop, and only then is the window
    // read. This is what SerializeCapture does, and it is the case that must be
    // clean under tsan.
    constexpr std::uint64_t kCapacity = 64;
    std::vector<Event>      storage(kCapacity);
    Detail::EventRing       ring;
    ring.Reset(storage.data(), kCapacity);

    std::atomic<bool> keepWriting{true};
    std::atomic<bool> writerStopped{false};

    std::thread writer(
        [&]
        {
            std::uint64_t seed = 1;
            while (keepWriting.load(std::memory_order_relaxed))
            {
                ring.Push(SelfCheckingEvent(seed++));
            }
            writerStopped.store(true, std::memory_order_release);
        });

    for (std::int32_t round = 0; round < 50; ++round)
    {
        std::this_thread::yield();
    }

    // Stop, then wait for the producer to actually be gone before sampling —
    // pausing and reading in the same breath would let a thread that has not yet
    // observed the pause lap the reader, which the one-slot sacrifice does not
    // protect against.
    keepWriting.store(false, std::memory_order_relaxed);
    while (!writerStopped.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    writer.join();

    const std::uint64_t cursor = ring.WriteCursor();
    const std::uint64_t begin  = ring.ReadableBegin(cursor);
    REQUIRE(cursor > kCapacity); // The run must actually have wrapped for this to mean anything.

    std::uint64_t previous = 0;
    for (std::uint64_t index = begin; index < cursor; ++index)
    {
        const Event &event = ring.At(index);
        CHECK(IsIntact(event));
        CHECK(event.timestampTicks > previous); // Monotonic: no stale slot resurfaced.
        previous = event.timestampTicks;
    }
}
