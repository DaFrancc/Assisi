#pragma once

#ifndef MESHDATA_HPP
#define MESHDATA_HPP

#include <Assisi/Math/GLM.hpp>

#include <vector>

namespace Assisi::Render
{
struct Vertex
{
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Normal{0.0f, 0.0f, 1.0f};
    glm::vec2 TextureCoordinates{0.0f, 0.0f};
};

struct MeshData
{
    std::vector<Vertex> Vertices;
    std::vector<unsigned int> Indices;
};
} /* namespace Assisi::Render */

#endif /* MESHDATA_HPP */