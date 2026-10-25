/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Core/EventQueue.hpp
/// @brief Per-frame typed event queue.
///
/// Events are plain data structs written by producer systems and read by
/// consumer systems later in the same frame. No phase reads an event twice, at
/// any frame rate, though a tick may run zero, one or several times a frame.
///
/// @par Frame ordering
/// @code
/// // One render frame, as Application::Run drives it:
///
///   input, the UI's clicks   → a click's event is pushed here
///   PreUpdate                → MarkTicks() after it
///   each tick, 0 or more:      BeginTick() … EndTick()
///     FixedUpdate, physics, PostFixedUpdate
///   Update, Ui, the frame's sounds
///   draw
///   EndFrame()
/// @endcode
///
/// - **Outside a tick** a phase reads what was pushed earlier in the same frame.
/// - **A tick** reads what was pushed since the previous tick ended: the
///   clicks and PreUpdate's events for the frame's first, and what the tick
///   itself pushed. Never an earlier tick's, nor Update's or Ui's.
/// - **EndFrame** drops everything, except on a frame that ran no tick: what
///   was pushed before MarkTicks then waits for the next tick, which reads it
///   once. The next frame's own phases never see it.
///
/// @par Access
/// Each world owns a queue, and the application owns one more for the events
/// declared `AEVENT(universal)`. None is a global: systems reach both through
/// @c ctx.events, an EventRoute that picks the queue by the event's type, so a
/// system cannot see another world's events. Tests construct throwaway queues
/// instead of sharing process state.
///
/// A queue knows which scope it holds. Pushing or reading an event of the
/// other scope is a bug, and asserts: a world event in the universal queue
/// reaches every world, and a universal one in a world's queue reaches nobody
/// looking for it.
///
/// @par Example
/// @code
/// // Define an event
/// AEVENT()
/// struct CollisionEvent { Entity a; Entity b; };
///
/// // Produce (e.g. from a system)
/// ctx.events.Push(CollisionEvent{entityA, entityB});
///
/// // Consume (e.g. in an Update system)
/// for (const CollisionEvent &e : ctx.events.Read<CollisionEvent>())
///     HandleCollision(e);
/// @endcode
///
/// The hosts mark and end each frame (App::BeginWorldFrame, App::StepWorld,
/// App::EndWorldFrame, and Application::Run for the universal queue). Call
/// Flush() manually in unit tests or custom loops.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/EventScope.hpp>

namespace Assisi::Core
{

/// @brief A checked, read-only view over one event type's queued events.
///
/// Returned by EventQueue::Read<E>(). Iterates and indexes like a span — and
/// converts to std::span<const E> — but in debug builds it also catches the
/// hazard the queue's storage invites: pushing another event of the same type E
/// while iterating this view can reallocate the backing vector and leave the
/// view dangling. Each Push/Clear bumps a per-type version counter; the view
/// snapshots it and asserts it is unchanged on every element access, turning a
/// silent use-after-realloc into a loud abort. Compiles down to a bare
/// pointer+size in release.
template <typename E> class EventSpan
{
public:
    EventSpan() = default;

    // `version` points at the queue's debug-only per-type counter, or is null in
    // release (Read passes nullptr there). The snapshot fields are always present
    // — they are a few bytes on a transient stack view — so the iterator/access
    // paths carry no #ifndef; the ASSISI_ASSERT that reads them drops to nothing
    // in release. A null version means "unchecked", handled by the assert itself.
    EventSpan(const E *data, std::size_t size, const std::uint32_t *version)
        : _data(data), _size(size), _version(version), _snapshot(version != nullptr ? *version : 0)
    {
    }

    struct Iterator
    {
        const E *ptr      = nullptr;
        const std::uint32_t *version  = nullptr; ///< Debug counter, or null (release / unchecked).
        std::uint32_t snapshot = 0;

        const E &operator*() const
        {
            ASSISI_ASSERT(version == nullptr || *version == snapshot,
                          "EventQueue::Read view invalidated: an event of the same type was Push()ed "
                          "while iterating it, which may have reallocated the backing vector. Copy the "
                          "events or defer the push until after the loop.");
            return *ptr;
        }
        Iterator &operator++()
        {
            // Check before advancing: a same-type Push in the loop body may have
            // reallocated the vector, leaving ptr dangling. Fail here rather than
            // do pointer arithmetic on a freed buffer.
            ASSISI_ASSERT(version == nullptr || *version == snapshot,
                          "EventQueue::Read view invalidated by a same-type Push mid-iteration.");
            ++ptr;
            return *this;
        }
        // Deliberately no invalidation check here: comparison only looks at the
        // snapshotted pointers, never the (possibly freed) buffer. The trade-off
        // is that an invalidating Push right before the loop's exit test can end
        // the loop instead of asserting — a missed detection, not UB;
        // operator*/operator++ catch it on any continued use.
        bool operator==(const Iterator &other) const { return ptr == other.ptr; }
        bool operator!=(const Iterator &other) const { return ptr != other.ptr; }
    };

    Iterator begin() const { return Iterator{_data, _version, _snapshot}; }
    Iterator end() const { return Iterator{_data + _size, _version, _snapshot}; }

    [[nodiscard]] std::size_t size() const { return _size; }
    [[nodiscard]] bool        empty() const { return _size == 0; }

    /// The raw pointer carries no version guard — it must be consumed before any
    /// further Push of this event type. Debug builds assert it has not already
    /// been invalidated at the point of the call.
    const E *data() const
    {
        ASSISI_ASSERT(_version == nullptr || *_version == _snapshot,
                      "EventQueue::Read view invalidated by a same-type Push before data() access.");
        return _data;
    }

    const E &operator[](std::size_t index) const
    {
        ASSISI_ASSERT(index < _size, "EventSpan index out of range.");
        ASSISI_ASSERT(_version == nullptr || *_version == _snapshot,
                      "EventQueue::Read view invalidated by a same-type Push before this access.");
        return _data[index];
    }

    /// Converts to an *unchecked* std::span over the same events: the span keeps
    /// no version guard, so element access through it is not protected the way
    /// this view's own iteration is. Convert only to hand the events to code that
    /// consumes them before any further Push of this event type. Debug builds
    /// assert the view is still valid at the moment of conversion.
    operator std::span<const E>() const
    {
        ASSISI_ASSERT(_version == nullptr || *_version == _snapshot,
                      "EventQueue::Read view invalidated by a same-type Push before converting to span.");
        return std::span<const E>(_data, _size);
    }

private:
    const E *_data     = nullptr;
    std::size_t _size     = 0;
    const std::uint32_t *_version  = nullptr; ///< Debug counter address (read by begin/end), null in release.
    std::uint32_t _snapshot = 0;
};

/// @brief Per-frame event queue.
///
/// Push events from any system; consume them with Read<E>() later in the frame.
/// The host ends each frame with EndFrame().
class EventQueue
{
public:
    /// @brief A queue holding events of @p scope, and only those.
    explicit EventQueue(EventScope scope = EventScope::World) : _scope(scope) {}

    [[nodiscard]] EventScope Scope() const { return _scope; }

    /// @brief Discards every Push made while it lives.
    ///
    /// A client holds one while it steps again ticks it has already stepped:
    /// whatever those ticks had to say was pushed when they ran live.
    class DropScope
    {
public:
        explicit DropScope(EventQueue &queue) : _queue(queue), _wasDropping(queue._dropping)
        {
            queue._dropping = true;
        }
        ~DropScope() { _queue._dropping = _wasDropping; }

        DropScope(const DropScope &) = delete;
        DropScope &operator=(const DropScope &) = delete;

private:
        EventQueue &_queue;
        bool _wasDropping;
    };

    /// @brief Where one reader's ticks have read the queue up to: its next tick
    /// sees what was pushed from there on. A world holds one for its own queue
    /// and one for the universal queue, which every world's ticks read.
    struct TickMark
    {
        std::uint64_t from = 0;
    };

    /// @brief Append an event of type E to its queue, unless a DropScope is
    /// holding it.
    template <typename E>
    void Push(E event)
    {
        ASSISI_ASSERT(ScopeOf<E>() == _scope,
                      "EventQueue: this event belongs in the other queue, a world's or the universal one. Push "
                      "through ctx.events, which picks the queue the event's declaration names.");
        if (_dropping)
        {
            return;
        }
        TypedQueue<E> &queue = GetOrCreate<E>();
        queue.events.push_back(std::move(event));
        queue.sequences.push_back(_next++);
#ifndef NDEBUG
        ++queue.version;
#endif
    }

    /// @brief Returns a checked view over the events of type E the current
    /// phase sees, in push order.
    ///
    /// Inside a tick (BeginTick to EndTick), what was pushed since the reader's
    /// previous tick ended; anywhere else, what was pushed this frame. The view
    /// is valid until the next EndFrame() or Flush() call. Returns an empty view
    /// if no events of this type were pushed.
    ///
    /// @warning The view points into the queue's internal vector. Pushing
    /// another event of the *same* type E while iterating it may reallocate that
    /// vector and leave the view dangling — the same hazard as mutating a
    /// container mid-range-for. Debug builds catch it: each Push/Clear bumps a
    /// per-type version the view checks on every element access, so a same-type
    /// push mid-iteration trips a loud assert instead of silent UB. Release
    /// builds do not check, so if a consumer needs to emit more E's while reading
    /// E's, copy the events first or defer the pushes until after the loop.
    template <typename E>
    EventSpan<E> Read() const
    {
        ASSISI_ASSERT(ScopeOf<E>() == _scope,
                      "EventQueue: this event is never in this queue, which holds the other scope. Read through "
                      "ctx.events, which picks the queue the event's declaration names.");
        auto it = _queues.find(typeid(E));
        if (it == _queues.end())
            return {};
        const TypedQueue<E> &queue = static_cast<const TypedQueue<E> &>(*it->second);
        const std::size_t first = queue.FirstFrom(_readFrom);
#ifndef NDEBUG
        return EventSpan<E>(queue.events.data() + first, queue.events.size() - first, &queue.version);
#else
        return EventSpan<E>(queue.events.data() + first, queue.events.size() - first, nullptr);
#endif
    }

    /// @brief The frame has reached its ticks. What was pushed before here waits
    /// for the next tick if the frame runs none.
    void MarkTicks() { _tickPoint = _next; }

    /// @brief Until EndTick, Read sees what was pushed since @p mark: since the
    /// end of this reader's previous tick. Ticks do not nest.
    void BeginTick(const TickMark &mark)
    {
        ASSISI_ASSERT(!_inTick, "EventQueue: a tick began inside another; end the first one before the next");
        _readFrom = mark.from;
        _inTick = true;
        _ticked = true;
    }

    /// @brief Ends the tick BeginTick began. @p mark moves past everything
    /// pushed so far, so the reader's next tick sees none of it, and Read sees
    /// the frame's events again.
    void EndTick(TickMark &mark)
    {
        ASSISI_ASSERT(_inTick, "EventQueue: a tick ended that had not begun");
        mark.from = _next;
        _readFrom = _frameStart;
        _inTick = false;
    }

    /// @brief Moves @p mark past everything pushed so far, so that reader's
    /// ticks never see it: for a reader that is not ticking, which keeps
    /// nothing for later.
    void SkipPending(TickMark &mark) const { mark.from = _next; }

    /// @brief Ends the frame. Every event is dropped, except, on a frame that
    /// ran no tick while @p keepPending, those pushed before MarkTicks: they
    /// wait for the next tick. Called by the host once a frame.
    ///
    /// What is kept is not seen by the next frame's own phases, which read only
    /// what that frame pushes. @p keepPending is false for a queue whose reader
    /// is not ticking, as a paused world is not, so nothing piles up while it
    /// waits.
    void EndFrame(bool keepPending)
    {
        const bool keep = keepPending && !_ticked;
        for (auto &[type, queue] : _queues)
        {
            if (keep)
            {
                queue->DropFrom(_tickPoint);
            }
            else
            {
                queue->Clear();
            }
        }
        _frameStart = _next;
        _readFrom = _next;
        _tickPoint = _next;
        _ticked = false;
    }

    /// @brief Clear all event queues: EndFrame keeping nothing. For tests and
    /// loops with no ticks.
    void Flush() { EndFrame(/*keepPending=*/ false); }

private:
    struct IQueue
    {
        virtual ~IQueue() = default;
        virtual void Clear() = 0;
        /// Drops the events pushed at or after @p sequence.
        virtual void DropFrom(std::uint64_t sequence) = 0;
    };

    template <typename E>
    struct TypedQueue : IQueue
    {
        std::vector<E> events;
        /// Each event's place in the queue's push order, ascending: what Read and
        /// EndFrame find a phase's events by.
        std::vector<std::uint64_t> sequences;
#ifndef NDEBUG
        std::uint32_t version = 0; ///< Bumped on every Push/Clear so a Read view can detect invalidation.
#endif
        /// The index of the first event pushed at or after @p sequence.
        [[nodiscard]] std::size_t FirstFrom(std::uint64_t sequence) const
        {
            return static_cast<std::size_t>(std::lower_bound(sequences.begin(), sequences.end(), sequence) -
                                            sequences.begin());
        }
        void Clear() override
        {
            events.clear();
            sequences.clear();
#ifndef NDEBUG
            ++version;
#endif
        }
        void DropFrom(std::uint64_t sequence) override
        {
            const std::size_t first = FirstFrom(sequence);
            events.erase(events.begin() + static_cast<std::ptrdiff_t>(first), events.end());
            sequences.erase(sequences.begin() + static_cast<std::ptrdiff_t>(first), sequences.end());
#ifndef NDEBUG
            ++version;
#endif
        }
    };

    template <typename E>
    TypedQueue<E> &GetOrCreate()
    {
        auto it = _queues.find(typeid(E));
        if (it != _queues.end())
            return static_cast<TypedQueue<E> &>(*it->second);

        auto result = _queues.emplace(typeid(E), std::make_unique<TypedQueue<E>>());
        return static_cast<TypedQueue<E> &>(*result.first->second);
    }

    std::unordered_map<std::type_index, std::unique_ptr<IQueue>> _queues;
    std::uint64_t _next = 0;       ///< The sequence the next push takes.
    std::uint64_t _frameStart = 0; ///< The first sequence this frame pushed.
    std::uint64_t _tickPoint = 0;  ///< Where MarkTicks put the frame's ticks.
    std::uint64_t _readFrom = 0;   ///< The first sequence Read sees now.
    EventScope _scope;
    bool _dropping = false; ///< A DropScope is alive.
    bool _inTick = false;   ///< Between BeginTick and EndTick.
    bool _ticked = false;   ///< A tick began this frame.
};

} // namespace Assisi::Core