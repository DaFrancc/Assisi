/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestComponents.hpp
/// @brief Replicated components a test's systems read and write on a host and
/// its clients.

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

} // namespace Assisi::App::Test
