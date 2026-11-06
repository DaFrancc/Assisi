/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LineVertices.hpp
/// @brief What the overlay's lines are made of, apart from the pass that draws
///        them, so building them needs no GPU.

#include <span>
#include <vector>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Editor
{

/// @brief One endpoint of a line segment: a world-space position and an RGBA
/// colour written straight to the scene target. Segments are drawn as a LineList,
/// so vertices come in consecutive pairs (0-1, 2-3, ...).
struct LineVertex
{
    glm::vec3 position;
    glm::vec4 color;
};

/// @brief One corner of a line drawn wider than a pixel: two triangles per
/// segment, each corner carrying both ends so the vertex shader can push it
/// sideways by a set number of pixels, whatever the distance.
struct ThickLineVertex
{
    glm::vec4 color;
    glm::vec3 position;  ///< This corner's end of the segment.
    glm::vec3 other;     ///< The segment's other end.
    float side = 0.f;    ///< Which side of the segment the corner is pushed to: -1 or +1.
    float halfWidth = 0.f; ///< How far it is pushed, in pixels.
};

/// @brief Appends the six corners that draw each segment of @p lines, a LineList,
/// @p widthPixels wide. A trailing unpaired vertex is dropped, as Draw drops it.
void AppendThickLines(std::vector<ThickLineVertex> &out, std::span<const LineVertex> lines, float widthPixels);

} // namespace Assisi::Editor
