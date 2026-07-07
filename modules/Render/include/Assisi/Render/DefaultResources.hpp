/*
 * Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")
 */

#pragma once

/// @file DefaultResources.hpp
/// @brief Engine-wide fallback GPU resources.
///
/// Provides access to shared default textures (e.g. the 1x1 white texture)
/// used when no explicit resource has been assigned to an object.

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{
/// @brief Provides access to built-in fallback GPU resources.
///
/// Only the albedo fallback exists so far — normal/metallic/roughness defaults
/// return once those material channels are wired up, see
/// docs/nvrhi-migration-todo.md.
class DefaultResources
{
  public:
    /// @brief Returns the engine-wide 1x1 white RGBA texture, creating it on first call.
    static nvrhi::ITexture *WhiteTexture(nvrhi::IDevice *device);
};
} /* namespace Assisi::Render */
