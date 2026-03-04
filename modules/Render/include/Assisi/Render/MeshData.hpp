#pragma once

#ifndef MESHDATA_HPP
#define MESHDATA_HPP

/// @file MeshData.hpp
/// @brief CPU-side mesh representation used as input to MeshBuffer::Upload().

#include <Assisi/Math/GLM.hpp>

#include <vector>

namespace Assisi::Render
{
/// @brief A single vertex with position, surface normal, and UV coordinates.
struct Vertex
{
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Normal{0.0f, 0.0f, 1.0f};
    glm::vec2 TextureCoordinates{0.0f, 0.0f};
};

/// @brief CPU-side mesh: a list of vertices and triangle indices.
struct MeshData
{
    std::vector<Vertex> Vertices;
    std::vector<unsigned int> Indices; ///< Triangle list; every 3 indices form one triangle.
};
} /* namespace Assisi::Render */

#endif /* MESHDATA_HPP */