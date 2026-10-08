/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AnimationPayload.hpp
/// @brief The cooked clip's bytes, written by the cook step and read by
///        LoadAnimation. Internal to Geometry.

#include <cstddef>
#include <vector>

#include <Assisi/Geometry/AnimationClip.hpp>

namespace Assisi::Geometry
{

/// @brief @p clip as the payload LoadAnimation reads.
[[nodiscard]] std::vector<std::byte> WriteAnimationPayload(const AnimationClip &clip);

} // namespace Assisi::Geometry
