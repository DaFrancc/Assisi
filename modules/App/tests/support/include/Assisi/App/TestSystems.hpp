/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestSystems.hpp
/// @brief Systems that exist to be named by a test level.
///
/// Declared with ASYSTEM rather than registered by hand, because that *is* the
/// thing under test: a declaration in a reflected header reaches the catalog by
/// being linked, and a level naming it gets it. Nothing calls a registration
/// function, which is the point — there is none to forget.
///
/// Defined inline, deliberately. assisi_link_reflections sweeps *every* generated
/// object into each final executable, so a header whose registrations reference
/// out-of-line functions would leave the game binaries with undefined symbols
/// for test code it has no reason to contain. Header-only keeps the registration
/// self-contained, which is the same reason the NetSync test components are.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::App::Test
{

/// How many times each system has run, per world, so a test can tell "installed
/// and running" from "installed". Keyed by world address rather than counted
/// globally, because one instance running over two worlds is exactly the bug the
/// per-world registry exists to prevent.
class RunCounts
{
public:
    static RunCounts &Instance()
    {
        static RunCounts counts;
        return counts;
    }

    void Reset() { _entries.clear(); }

    [[nodiscard]] std::uint32_t Count(const World &world, std::string_view system) const
    {
        for (const Entry &entry : _entries)
        {
            if (entry.world == &world && entry.system == system)
                return entry.runs;
        }
        return 0;
    }

    void Record(const World &world, std::string_view system)
    {
        for (Entry &entry : _entries)
        {
            if (entry.world == &world && entry.system == system)
            {
                ++entry.runs;
                return;
            }
        }
        _entries.push_back(Entry{.world = &world, .system = std::string{system}, .runs = 1});
    }

private:
    struct Entry
    {
        const World *world = nullptr;
        std::string system;
        std::uint32_t runs = 0;
    };
    std::vector<Entry> _entries;
};

/// The sequence systems ran in, across every world.
///
/// Counts cannot answer an ordering question: two systems that each ran once say
/// nothing about which went first. Separate from RunCounts rather than folded
/// into it, because a per-world tally and a flat sequence are different shapes
/// and one container doing both serves neither well.
class RunOrder
{
public:
    static RunOrder &Instance()
    {
        static RunOrder order;
        return order;
    }

    void Reset() { _names.clear(); }

    void Record(std::string_view name) { _names.emplace_back(name); }

    [[nodiscard]] const std::vector<std::string> &Names() const { return _names; }

private:
    std::vector<std::string> _names;
};

ASYSTEM(Update, name = "Counter") inline void CounterSystem(SystemContext &ctx);

/// Ordered after Counter, so a test can assert the graph was honoured rather
/// than that both merely ran.
ASYSTEM(Update, name = "Follower", after = Counter) inline void FollowerSystem(SystemContext &ctx);

/// One InputContext, N resident worlds — this is the flag that keeps a system
/// from applying the same keypresses in every one.
ASYSTEM(Update, name = "ActiveOnly", activeWorldOnly) inline void ActiveOnlySystem(SystemContext &ctx);

/// Turns contact reporting on for the world it runs in, which is what "the
/// system's own needs travel with the system" means in practice: a level that
/// names it gets the reporting too, without knowing it had to ask.
ASYSTEM(FixedUpdate, name = "Contacts") inline void ContactsSystem(SystemContext &ctx);

/// A pair ordered against each other in the Render phase.
///
/// Render systems reach a world through a different SystemRegistry call than
/// every other phase, so their `after`/`before` travels a path of its own and
/// needs a case of its own — the Update pair above exercises none of it.
ASYSTEM(Render, name = "DrawEarly") inline void DrawEarlySystem(RenderContext &ctx);
ASYSTEM(Render, name = "DrawLate", after = DrawEarly) inline void DrawLateSystem(RenderContext &ctx);

inline void CounterSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Counter");
}

inline void FollowerSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Follower");
}

inline void ActiveOnlySystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "ActiveOnly");
}

inline void ContactsSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Contacts");
}

inline void DrawEarlySystem(RenderContext &)
{
    RunOrder::Instance().Record("DrawEarly");
}

inline void DrawLateSystem(RenderContext &)
{
    RunOrder::Instance().Record("DrawLate");
}

} // namespace Assisi::App::Test
