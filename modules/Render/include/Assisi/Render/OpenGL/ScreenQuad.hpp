#pragma once

/// @file ScreenQuad.hpp
/// @brief Fullscreen quad VAO for post-process passes.
///
/// Covers NDC [-1, 1]² with UVs [0, 1]². Bind a shader and call Draw().

#include <glad/glad.h>

namespace Assisi::Render::OpenGL
{

/// @brief Owns a VAO/VBO for a fullscreen quad (two triangles).
///
/// Vertex layout:
///   - Attrib 0 — Position  (vec2, NDC)
///   - Attrib 1 — TexCoords (vec2, 0..1)
///
/// Move-only; copying is disabled.
class ScreenQuad
{
  public:
    ScreenQuad() { Create(); }
    ~ScreenQuad() { Destroy(); }

    ScreenQuad(const ScreenQuad &) = delete;
    ScreenQuad &operator=(const ScreenQuad &) = delete;

    ScreenQuad(ScreenQuad &&other) noexcept
        : _vao(other._vao), _vbo(other._vbo)
    {
        other._vao = other._vbo = 0;
    }

    ScreenQuad &operator=(ScreenQuad &&other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            _vao = other._vao;
            _vbo = other._vbo;
            other._vao = other._vbo = 0;
        }
        return *this;
    }

    /// @brief Draws the fullscreen quad (6 vertices, no index buffer).
    void Draw() const
    {
        glBindVertexArray(_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

  private:
    void Create()
    {
        // clang-format off
        static constexpr float kVertices[] = {
            // pos (xy)   // uv
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
        };
        // clang-format on

        glGenVertexArrays(1, &_vao);
        glGenBuffers(1, &_vbo);
        glBindVertexArray(_vao);
        glBindBuffer(GL_ARRAY_BUFFER, _vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(0));

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                              reinterpret_cast<void *>(2 * sizeof(float)));

        glBindVertexArray(0);
    }

    void Destroy()
    {
        if (_vbo != 0) { glDeleteBuffers(1, &_vbo);      _vbo = 0; }
        if (_vao != 0) { glDeleteVertexArrays(1, &_vao); _vao = 0; }
    }

    unsigned int _vao = 0;
    unsigned int _vbo = 0;
};

} // namespace Assisi::Render::OpenGL