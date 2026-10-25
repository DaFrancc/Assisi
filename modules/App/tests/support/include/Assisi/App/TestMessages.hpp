/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestMessages.hpp
/// @brief Messages a test's systems send through ctx.net and read back as
/// Received<T>.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>

#include <cstdint>

namespace Assisi::App::Test
{

/// @brief A player asking the host to ring a bell, through the character
/// ringing it.
AMSG(intent, reliable)
struct TestAskBell
{
    AFIELD(controlled) Assisi::ECS::Entity ringer;
    AFIELD() int32_t bell = 0;
};

/// @brief The host telling everyone a bell rang.
AMSG(event, reliable, independent)
struct TestBellRang
{
    AFIELD() int32_t bell = 0;
};

} // namespace Assisi::App::Test
