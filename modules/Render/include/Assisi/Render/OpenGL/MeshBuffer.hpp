#pragma once

/// @file MeshBuffer.hpp
/// @brief GPU-side mesh storage backed by OpenGL buffer objects.
///
/// `MeshBuffer` owns a VAO, VBO, and EBO.  Upload CPU-side `MeshData` once;
/// then Bind() before issuing draw calls.  Move-only; copying is disabled.

#include <glad/glad.h>

#include <Assisi/Math/GLM.hpp>

#include <Assisi/Render/MeshData.hpp>

namespace Assisi::Render::OpenGL
{
/// @brief RAII owner of an OpenGL VAO/VBO/EBO triple for a single mesh.
///
/// Vertex layout bound at upload time:
/// - Attrib 0 — Position  (vec3)
/// - Attrib 1 — Normal    (vec3)
/// - Attrib 2 — TexCoords (vec2)
class MeshBuffer
{
  public:
    MeshBuffer() = default;

    /// @brief Uploads mesh data to the GPU immediately.
    explicit MeshBuffer(const Assisi::Render::MeshData &meshData) { Upload(meshData); }

    ~MeshBuffer() { Destroy(); }

    MeshBuffer(const MeshBuffer &) = delete;
    MeshBuffer &operator=(const MeshBuffer &) = delete;

    /// @brief Transfers GPU buffer ownership; the moved-from object becomes empty.
    MeshBuffer(MeshBuffer &&other) noexcept
        : _vertexArrayObject(other._vertexArrayObject), _vertexBufferObject(other._vertexBufferObject),
          _elementBufferObject(other._elementBufferObject), _indexCount(other._indexCount)
    {
        other._vertexArrayObject = 0;
        other._vertexBufferObject = 0;
        other._elementBufferObject = 0;
        other._indexCount = 0;
    }

    /// @brief Destroys the current buffers then takes ownership from `other`.
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

    /// @brief Destroys any existing GPU buffers and uploads new mesh data.
    ///
    /// Configures the VAO with the standard Assisi vertex attribute layout
    /// (Position at 0, Normal at 1, TextureCoordinates at 2).
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

        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Assisi::Render::Vertex),
                              reinterpret_cast<void *>(sizeof(glm::vec3) + sizeof(glm::vec3) + sizeof(glm::vec2)));

        glBindVertexArray(0);
    }

    /// @brief Binds the VAO, making it active for subsequent draw calls.
    void Bind() const { glBindVertexArray(_vertexArrayObject); }

    /// @brief Returns the number of indices to pass to `glDrawElements`.
    unsigned int IndexCount() const { return _indexCount; }

  private:
    /// @brief Deletes all GPU objects and resets handles to zero.
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
    unsigned int _indexCount = 0; ///< Cached index count for draw calls.
};
} /* namespace Assisi::Render::OpenGL */
