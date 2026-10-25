/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestMessages.hpp
/// @brief A message only Core's tests declare, so receiving one is tested
/// where it is defined, without the network.

#include <Assisi/Prelude.hpp>

#include <cstdint>

namespace Assisi::Core::Test
{

/// @brief A player asking the host to ring a bell.
AMSG(intent, reliable)
struct TestRingBell
{
    AFIELD() int32_t bell = 0;
    AFIELD() int32_t loudness = 1;
};

} // namespace Assisi::Core::Test
