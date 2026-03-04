/*
 * Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")
 */

#pragma once

/// @file DefaultResources.hpp
/// @brief Engine-wide fallback GPU resources.
///
/// Provides access to shared default assets (e.g. the 1×1 white texture)
/// that are used when no explicit resource has been assigned to an object.
/// Requires the render backend to be initialized before use.

namespace Assisi::Render
{
/// @brief Provides access to built-in fallback GPU resources.
class DefaultResources
{
  public:
    /// @brief Returns the OpenGL ID of the engine-wide 1×1 white RGBA texture.
    ///
    /// @pre The render backend must be initialized before calling this.
    static unsigned int WhiteTextureId();
};
} /* namespace Assisi::Render */