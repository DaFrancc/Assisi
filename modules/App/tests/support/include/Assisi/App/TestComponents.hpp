/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestComponents.hpp
/// @brief Components a test's systems and screens read and write: replicated
/// ones on a host and its clients, and others a screen binds to.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>

namespace Assisi::App::Test
{

/// @brief An item a player holds at a joint of their body, as the sample
/// game's Holdable: the host names the holder, every machine hangs it.
ACOMP(replicable)
struct TestHoldable
{
    /// The character holding it, or NullEntity while it lies free.
    AFIELD() Assisi::ECS::Entity holder{Assisi::ECS::NullEntity};
};

/// @brief How much health something has left, for a nameplate's bar to bind.
ACOMP()
struct TestHealth
{
    AFIELD() float current = 0.f;
};

} // namespace Assisi::App::Test
