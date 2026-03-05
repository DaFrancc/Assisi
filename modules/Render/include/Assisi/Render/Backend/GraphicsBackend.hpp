#pragma once

/// @file GraphicsBackend.hpp
/// @brief Enumeration of supported graphics backends.

namespace Assisi::Render::Backend
{
/// @brief Selects which graphics API the RenderSystem initializes.
enum class GraphicsBackend
{
    None,   ///< No backend; rendering is disabled.
    OpenGL, ///< OpenGL backend (currently implemented).
    Vulkan  ///< Vulkan backend (not yet implemented).
};
} // namespace Assisi::Render::Backend
