/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestStartContext.hpp
/// @brief The context the one-shot phases run under, for tests that drive them
///        directly.
///
/// One definition rather than one per test file, for the reason the hosts build
/// theirs identically: a Begin system must not be able to tell who started it.
/// A copy that drifted would test a context no host ever passes.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>

namespace Assisi::App::Test
{

/// @brief What BeginWorld, SettleWorld and DrainSystemInstalls are handed: the
/// world, and nothing a frame would supply.
///
/// The queue is shared across every caller because nothing reads back out of it
/// here; what is being asserted is which systems ran, not what they published.
inline SystemContext StartContext(World &world)
{
    static Core::EventQueue events;
    return {.world         = world,
            .dt            = 0.f,
            .simTick       = 0,
            .input         = nullptr,
            .actions       = nullptr,
            .events        = events,
            .isActiveWorld = true,
            .worlds        = nullptr};
}

} // namespace Assisi::App::Test
