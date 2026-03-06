#pragma once

/// @file DefaultTextures.hpp
/// @brief Lazily-created built-in OpenGL textures.

#include <glad/glad.h>

namespace Assisi::Render::OpenGL
{
/// @brief Provides lazily-initialized single-pixel fallback textures.
class DefaultTextures
{
  public:
    /// @brief Returns the OpenGL ID of a 1×1 fully-opaque white RGBA texture.
    ///
    /// Created on first call and cached for the lifetime of the process.
    /// Useful as a neutral diffuse texture that lets shader tint colour
    /// pass through unmodified.
    ///
    /// @pre A valid OpenGL context must be current.
    static unsigned int WhiteTexture()
    {
        static unsigned int whiteTextureIdentifier = 0;

        if (whiteTextureIdentifier != 0)
        {
            return whiteTextureIdentifier;
        }

        /* A single white pixel (RGBA). */
        const unsigned char whitePixel[4] = {255, 255, 255, 255};

        glGenTextures(1, &whiteTextureIdentifier);
        glBindTexture(GL_TEXTURE_2D, whiteTextureIdentifier);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);

        glBindTexture(GL_TEXTURE_2D, 0);

        return whiteTextureIdentifier;
    }

    /// @brief Returns the OpenGL ID of a 1×1 flat normal-map texture (128, 128, 255, 255).
    ///
    /// Decodes to tangent-space normal (0, 0, 1) — use as a no-op normal map fallback.
    static unsigned int FlatNormalTexture()
    {
        static unsigned int id = 0;
        if (id != 0)
        {
            return id;
        }

        const unsigned char pixel[4] = {128, 128, 255, 255};
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindTexture(GL_TEXTURE_2D, 0);
        return id;
    }

    /// @brief Returns the OpenGL ID of a 1×1 black RGBA texture (0, 0, 0, 255).
    ///
    /// Use as a metallic fallback (fully dielectric — metallic = 0).
    static unsigned int BlackTexture()
    {
        static unsigned int id = 0;
        if (id != 0)
        {
            return id;
        }

        const unsigned char pixel[4] = {0, 0, 0, 255};
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindTexture(GL_TEXTURE_2D, 0);
        return id;
    }

    /// @brief Returns the OpenGL ID of a 1×1 mid-grey RGBA texture (128, 128, 128, 255).
    ///
    /// Decodes to ~0.5 roughness — use as a roughness fallback.
    static unsigned int GreyTexture()
    {
        static unsigned int id = 0;
        if (id != 0)
        {
            return id;
        }

        const unsigned char pixel[4] = {128, 128, 128, 255};
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glBindTexture(GL_TEXTURE_2D, 0);
        return id;
    }
};
} /* namespace Assisi::Render::OpenGL */