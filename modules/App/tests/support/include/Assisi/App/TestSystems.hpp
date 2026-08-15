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
/// out-of-line functions would leave the sandbox binary with undefined symbols
/// for test code it has no reason to contain. Header-only keeps the registration
/// self-contained, which is the same reason the NetSync test components are.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>
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

ASYSTEM(Update) inline void CounterSystem(SystemContext &ctx);

/// Ordered after Counter, so a test can assert the graph was honoured rather
/// than that both merely ran.
ASYSTEM(Update, after = Counter) inline void FollowerSystem(SystemContext &ctx);

/// One InputContext, N resident worlds — this is the flag that keeps a system
/// from applying the same keypresses in every one.
ASYSTEM(Update, name = "ActiveOnly", activeWorldOnly) inline void ActiveOnlySystem(SystemContext &ctx);

/// Turns contact reporting on for the world it runs in, which is what "the
/// system's own needs travel with the system" means in practice: a level that
/// names it gets the reporting too, without knowing it had to ask.
ASYSTEM(FixedUpdate) inline void ContactsSystem(SystemContext &ctx);

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
    // Its own need, turned on where the need is. Costs one branch per fixed step,
    // and survives a world that turns it back off.
    if (!ctx.world.physics.IsContactReporting())
        ctx.world.physics.SetContactReporting(true);

    RunCounts::Instance().Record(ctx.world, "Contacts");
}

} // namespace Assisi::App::Test
