/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Core/EventQueue.hpp
/// @brief Per-frame typed event queue.
///
/// Events are plain data structs written by producer systems and read by
/// consumer systems within the same frame.  The Application flushes all
/// queues once per render frame, so events do not persist across frames.
///
/// @par Frame ordering
/// @code
/// // One render frame:
///
///   glfwPollEvents()    ← GLFW dispatches window/input callbacks
///   _input->Poll()      ← InputContext updated; input state is now fresh
///
///   OnFixedUpdate (may run N times):
///     PhysicsStep     → Push(CollisionEvent{a, b})
///     PhysicsSyncTransforms
///
///   OnUpdate:
///     PreUpdate       → input is available; first chance to react this frame
///     Update          → DamageSystem: Read<CollisionEvent>()  ← visible, same frame as FixedUpdate
///     PostUpdate      → CleanupSystem: Read<DestroyRequestEvent>()  ← visible, pushed in Update
///
///   RenderFrame + OnImGui
///     (events pushed here are NOT visible to systems this frame —
///      they are flushed at end of frame before systems run again)
///
///   EventQueue::Flush()  ← all queues cleared
/// @endcode
///
/// Rule of thumb: an event pushed in phase X is readable in any phase that
/// runs AFTER X within the same frame, before Flush().
///
/// @par Access
/// The queue is owned by Application and reached through SystemContext, not a
/// global — systems receive it as @c ctx.events, and Application exposes it to
/// derived apps via GetEvents(). This keeps the frame-loop ordering explicit
/// and lets tests construct a throwaway queue instead of sharing process state.
///
/// @par Example
/// @code
/// // Define an event (annotate with AEVENT() to mark intent)
/// AEVENT()
/// struct CollisionEvent { Entity a; Entity b; };
///
/// // Produce (e.g. from a system)
/// ctx.events.Push(CollisionEvent{entityA, entityB});
///
/// // Consume (e.g. in a PostUpdate system)
/// for (const auto& e : ctx.events.Read<CollisionEvent>())
///     HandleCollision(e);
/// @endcode
///
/// Flushing is handled automatically by Application::Run() at the end of
/// each render frame.  Call Flush() manually in unit tests or custom loops.

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <typeindex>
#include <unordered_map>
#include <vector>

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

    EventSpan(const E *data, std::size_t size, [[maybe_unused]] const std::uint32_t *version)
        : _data(data), _size(size)
    {
#ifndef NDEBUG
        _version  = version;
        _snapshot = (version != nullptr) ? *version : 0;
#endif
    }

    struct Iterator
    {
        const E *ptr = nullptr;
#ifndef NDEBUG
        const std::uint32_t *version  = nullptr;
        std::uint32_t        snapshot = 0;
#endif
        const E &operator*() const
        {
#ifndef NDEBUG
            assert((version == nullptr || *version == snapshot) &&
                   "EventQueue::Read view invalidated: an event of the same type was Push()ed while "
                   "iterating it, which may have reallocated the backing vector. Copy the events or "
                   "defer the push until after the loop.");
#endif
            return *ptr;
        }
        Iterator &operator++()
        {
            ++ptr;
            return *this;
        }
        bool operator==(const Iterator &other) const { return ptr == other.ptr; }
        bool operator!=(const Iterator &other) const { return ptr != other.ptr; }
    };

    Iterator begin() const
    {
#ifndef NDEBUG
        return Iterator{_data, _version, _snapshot};
#else
        return Iterator{_data};
#endif
    }
    Iterator end() const
    {
#ifndef NDEBUG
        return Iterator{_data + _size, _version, _snapshot};
#else
        return Iterator{_data + _size};
#endif
    }

    [[nodiscard]] std::size_t size() const { return _size; }
    [[nodiscard]] bool        empty() const { return _size == 0; }
    const E                  *data() const { return _data; }

    const E &operator[](std::size_t index) const
    {
#ifndef NDEBUG
        assert((_version == nullptr || *_version == _snapshot) &&
               "EventQueue::Read view invalidated by a same-type Push before this access.");
#endif
        return _data[index];
    }

    operator std::span<const E>() const { return std::span<const E>(_data, _size); }

  private:
    const E    *_data = nullptr;
    std::size_t _size = 0;
#ifndef NDEBUG
    const std::uint32_t *_version  = nullptr;
    std::uint32_t        _snapshot = 0;
#endif
};

/// @brief Per-frame event queue.
///
/// Push events from any system; consume them with Read<E>() before the frame
/// ends.  All queues are cleared by Flush() once per frame.
class EventQueue
{
  public:
    /// @brief Append an event of type E to its queue.
    template <typename E>
    void Push(E event)
    {
        TypedQueue<E> &queue = GetOrCreate<E>();
        queue.events.push_back(std::move(event));
#ifndef NDEBUG
        ++queue.version;
#endif
    }

    /// @brief Returns a checked view over all events of type E queued this frame.
    ///
    /// The view is valid until the next Flush() call.
    /// Returns an empty view if no events of this type were pushed.
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
        auto it = _queues.find(typeid(E));
        if (it == _queues.end())
            return {};
        const TypedQueue<E> &queue = static_cast<const TypedQueue<E> &>(*it->second);
#ifndef NDEBUG
        return EventSpan<E>(queue.events.data(), queue.events.size(), &queue.version);
#else
        return EventSpan<E>(queue.events.data(), queue.events.size(), nullptr);
#endif
    }

    /// @brief Clear all event queues.  Called by Application once per frame.
    void Flush()
    {
        for (auto &[type, queue] : _queues)
            queue->Clear();
    }

  private:
    struct IQueue
    {
        virtual ~IQueue() = default;
        virtual void Clear() = 0;
    };

    template <typename E>
    struct TypedQueue : IQueue
    {
        std::vector<E> events;
#ifndef NDEBUG
        std::uint32_t version = 0; ///< Bumped on every Push/Clear so a Read view can detect invalidation.
#endif
        void Clear() override
        {
            events.clear();
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
};

} // namespace Assisi::Core