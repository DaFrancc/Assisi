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
/// @par Example
/// @code
/// // Define an event (annotate with AEVENT() to mark intent)
/// AEVENT()
/// struct CollisionEvent { Entity a; Entity b; };
///
/// // Produce (e.g. from the physics system)
/// EventQueue::Instance().Push(CollisionEvent{entityA, entityB});
///
/// // Consume (e.g. in a PostUpdate system)
/// for (const auto& e : EventQueue::Instance().Read<CollisionEvent>())
///     HandleCollision(e);
/// @endcode
///
/// Flushing is handled automatically by Application::Run() at the end of
/// each render frame.  Call Flush() manually in unit tests or custom loops.

#include <memory>
#include <span>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace Assisi::Core
{

/// @brief Global per-frame event queue.
///
/// Push events from any system; consume them with Read<E>() before the frame
/// ends.  All queues are cleared by Flush() once per frame.
class EventQueue
{
  public:
    static EventQueue &Instance();

    /// @brief Append an event of type E to its queue.
    template <typename E>
    void Push(E event)
    {
        GetOrCreate<E>().events.push_back(std::move(event));
    }

    /// @brief Returns a view over all events of type E queued this frame.
    ///
    /// The span is valid until the next Flush() call.
    /// Returns an empty span if no events of this type were pushed.
    template <typename E>
    std::span<const E> Read() const
    {
        auto it = _queues.find(typeid(E));
        if (it == _queues.end())
            return {};
        return static_cast<const TypedQueue<E> &>(*it->second).events;
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
        void Clear() override { events.clear(); }
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