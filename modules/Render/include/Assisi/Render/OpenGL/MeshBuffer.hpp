#pragma once

#ifndef MESHBUFFER_HPP
#define MESHBUFFER_HPP

#include <glad/glad.h>

#include <Assisi/Math/GLM.hpp>

#include <Assisi/Render/MeshData.hpp>

namespace Assisi::Render::OpenGL
{
class MeshBuffer
{
  public:
    MeshBuffer() = default;

    explicit MeshBuffer(const Assisi::Render::MeshData &meshData) { Upload(meshData); }

    ~MeshBuffer() { Destroy(); }

    MeshBuffer(const MeshBuffer &) = delete;
    MeshBuffer &operator=(const MeshBuffer &) = delete;

    MeshBuffer(MeshBuffer &&other) noexcept
        : _vertexArrayObject(other._vertexArrayObject), _vertexBufferObject(other._vertexBufferObject),
          _elementBufferObject(other._elementBufferObject), _indexCount(other._indexCount)
    {
        other._vertexArrayObject = 0;
        other._vertexBufferObject = 0;
        other._elementBufferObject = 0;
        other._indexCount = 0;
    }

    MeshBuffer &operator=(MeshBuffer &&other) noexcept
    {
        if (this != &other)
        {
            Destroy();

            _vertexArrayObject = other._vertexArrayObject;
            _vertexBufferObject = other._vertexBufferObject;
            _elementBufferObject = other._elementBufferObject;
            _indexCount = other._indexCount;

            other._vertexArrayObject = 0;
            other._vertexBufferObject = 0;
            other._elementBufferObject = 0;
            other._indexCount = 0;
        }

        return *this;
    }

    void Upload(const Assisi::Render::MeshData &meshData)
    {
        Destroy();

        _indexCount = static_cast<unsigned int>(meshData.Indices.size());

        glGenVertexArrays(1, &_vertexArrayObject);
        glGenBuffers(1, &_vertexBufferObject);
        glGenBuffers(1, &_elementBufferObject);

        glBindVertexArray(_vertexArrayObject);

        glBindBuffer(GL_ARRAY_BUFFER, _vertexBufferObject);
        glBufferData(GL_ARRAY_BUFFER, static_cast<long long>(meshData.Vertices.size() * sizeof(Assisi::Render::Vertex)),
                     meshData.Vertices.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _elementBufferObject);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<long long>(meshData.Indices.size() * sizeof(unsigned int)),
                     meshData.Indices.data(), GL_STATIC_DRAW);

        /* Vertex layout: Position (0), Normal (1), TextureCoordinates (2). */
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Assisi::Render::Vertex), reinterpret_cast<void *>(0));

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Assisi::Render::Vertex),
                              reinterpret_cast<void *>(sizeof(glm::vec3)));

        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Assisi::Render::Vertex),
                              reinterpret_cast<void *>(sizeof(glm::vec3) + sizeof(glm::vec3)));

        glBindVertexArray(0);
    }

    void Bind() const { glBindVertexArray(_vertexArrayObject); }

    unsigned int IndexCount() const { return _indexCount; }

  private:
    void Destroy()
    {
        if (_elementBufferObject != 0)
        {
            glDeleteBuffers(1, &_elementBufferObject);
            _elementBufferObject = 0;
        }

        if (_vertexBufferObject != 0)
        {
            glDeleteBuffers(1, &_vertexBufferObject);
            _vertexBufferObject = 0;
        }

        if (_vertexArrayObject != 0)
        {
            glDeleteVertexArrays(1, &_vertexArrayObject);
            _vertexArrayObject = 0;
        }

        _indexCount = 0;
    }

  private:
    unsigned int _vertexArrayObject = 0;
    unsigned int _vertexBufferObject = 0;
    unsigned int _elementBufferObject = 0;
    unsigned int _indexCount = 0;
};
} /* namespace Assisi::Render::OpenGL */

#endif /* MESHBUFFER_HPP */