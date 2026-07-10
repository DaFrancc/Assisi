/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetPath.hpp
/// @brief Virtual asset path — a TrivialString at the default capacity.

#include <cstddef>

#include <Assisi/Core/TrivialString.hpp>

namespace Assisi::Core
{

/// @brief Maximum number of characters an AssetPath can hold (128-byte value).
inline constexpr std::size_t kAssetPathMax = kDefaultTrivialStringCapacity;

/// @brief A virtual asset path (e.g. "textures/crate.png", "prim://cube").
///
/// Just a TrivialString at the asset-path capacity: heap-free, trivially
/// copyable, and safe to store inline in a component. reflectgen keys on the
/// spelled type name `Assisi::Core::AssetPath`, so the alias serializes as a
/// string like any other AssetPath field.
using AssetPath = TrivialString<kAssetPathMax>;

} // namespace Assisi::Core
