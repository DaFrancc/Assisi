/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DrawList.hpp
/// @brief What the UI hands the renderer each frame: quads in window pixels.
///
/// Plain data with no GPU types, so the core can build and test it headless and
/// the engine layer is the only thing that knows how it reaches the screen.

#include <cstdint>
#include <span>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief A rectangle in window pixels, origin top-left, y down.
struct Rect
{
    float x      = 0.f;
    float y      = 0.f;
    float width  = 0.f;
    float height = 0.f;
};

/// @brief A display-space colour with straight (not premultiplied) alpha. The
/// pass premultiplies, so authored colours read the way they are written.
struct Color
{
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
};

/// @brief The size of the surface the UI lays out against, in pixels.
struct Extent
{
    uint32_t width  = 0;
    uint32_t height = 0;
};

/// @brief One filled rectangle.
struct DrawQuad
{
    Rect rect;
    Color color;
};

/// @brief The quads to draw this frame, back to front.
class DrawList
{
public:
    void Clear() { _quads.clear(); }
    void Push(const DrawQuad &quad) { _quads.push_back(quad); }
    [[nodiscard]] std::span<const DrawQuad> Quads() const { return _quads; }

private:
    std::vector<DrawQuad> _quads;
};

} // namespace Assisi::Mondrian
