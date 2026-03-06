#pragma once

/// @file Texture2D.hpp
/// @brief RAII OpenGL 2D texture loaded from the asset system via stb_image.
///
/// Move-only. Upload once, then call Bind(slot) before draw calls.

#include <glad/glad.h>

#include <Assisi/Core/AssetSystem.hpp>

#include <expected>
#include <string_view>

namespace Assisi::Render::OpenGL
{
/// @brief Owns an OpenGL 2D texture object loaded from an image file.
class Texture2D
{
  public:
    Texture2D() = default;
    ~Texture2D() { Destroy(); }

    Texture2D(const Texture2D &) = delete;
    Texture2D &operator=(const Texture2D &) = delete;

    Texture2D(Texture2D &&other) noexcept : _textureId(other._textureId) { other._textureId = 0u; }

    Texture2D &operator=(Texture2D &&other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            _textureId = other._textureId;
            other._textureId = 0u;
        }
        return *this;
    }

    /// @brief Loads an image from a virtual asset path and uploads it to the GPU.
    ///
    /// Destroys any previously loaded texture first.
    /// Pixels are flipped vertically to match OpenGL's bottom-left origin.
    ///
    /// @return Success, or an AssetError if the file cannot be resolved/read.
    std::expected<void, Assisi::Core::AssetError> LoadFromAssets(std::string_view vpath) noexcept;

    /// @brief Binds the texture to the given texture unit.
    void Bind(unsigned int slot = 0u) const
    {
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(GL_TEXTURE_2D, _textureId);
    }

    /// @brief Raw OpenGL texture ID (0 if not loaded).
    unsigned int TextureId() const { return _textureId; }

    /// @brief Returns true if the texture is loaded and valid.
    bool IsValid() const { return _textureId != 0u; }

  private:
    void Destroy() noexcept
    {
        if (_textureId != 0u)
        {
            glDeleteTextures(1, &_textureId);
            _textureId = 0u;
        }
    }

    unsigned int _textureId = 0u;
};
} // namespace Assisi::Render::OpenGL