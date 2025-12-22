#pragma once

#ifndef GRAPHICSBACKEND_HPP
#define GRAPHICSBACKEND_HPP

namespace Assisi::Render::Backend
{
enum class GraphicsBackend
{
    None,
    OpenGL,
    Vulkan
};
} // namespace Assisi::Render::Backend

#endif /* GRAPHICSBACKEND_HPP */