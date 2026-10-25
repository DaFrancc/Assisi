/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file EventRoute.hpp
/// @brief A world's events and the whole program's, behind one Push and one
/// Read that pick the queue by the event's type.
///
/// What a system holds as `ctx.events`, and what a screen's controls push
/// through. Neither has to know which events are universal: the declaration
/// says, and the route asks it on every push and read, so an event cannot be
/// put where its readers will not look.

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <utility>

namespace Assisi::Core
{

class EventRoute
{
public:
    /// @brief Discards every push made through the route, into either queue,
    /// while it lives.
    ///
    /// A client holds one while it steps again ticks it has already stepped:
    /// whatever those ticks had to say was pushed when they ran live.
    class DropScope
    {
public:
        explicit DropScope(const EventRoute &route) : _world(route._world), _universal(route._universal) {}

private:
        EventQueue::DropScope _world;
        EventQueue::DropScope _universal;
    };

    /// @brief Routes world events to @p world, the queue of the world this
    /// route serves, and universal ones to @p universal. Both must outlive it.
    EventRoute(EventQueue &world, EventQueue &universal) : _world(world), _universal(universal)
    {
        ASSISI_ASSERT(world.Scope() == EventScope::World, "EventRoute: the world's queue holds universal events");
        ASSISI_ASSERT(universal.Scope() == EventScope::Universal,
                      "EventRoute: the universal queue is a world's queue");
    }

    /// @brief Appends @p event to the queue its type belongs in.
    template <typename E> void Push(E event) const { QueueFor<E>().Push(std::move(event)); }

    /// @brief Every event of type E pushed this frame into the queue its type
    /// belongs in. See EventQueue::Read for how long the view lasts.
    template <typename E> [[nodiscard]] EventSpan<E> Read() const { return QueueFor<E>().template Read<E>(); }

    /// @brief Where one world's ticks have read both its queues up to.
    struct TickMarks
    {
        EventQueue::TickMark world;
        EventQueue::TickMark universal;
    };

    /// @brief One tick of the world this route serves, in both queues: see
    /// EventQueue::BeginTick.
    void BeginTick(const TickMarks &marks) const
    {
        _world.BeginTick(marks.world);
        _universal.BeginTick(marks.universal);
    }

    /// @brief See EventQueue::EndTick.
    void EndTick(TickMarks &marks) const
    {
        _world.EndTick(marks.world);
        _universal.EndTick(marks.universal);
    }

private:
    template <typename E> [[nodiscard]] EventQueue &QueueFor() const
    {
        return ScopeOf<E>() == EventScope::Universal ? _universal : _world;
    }

    EventQueue &_world;
    EventQueue &_universal;
};

} // namespace Assisi::Core
