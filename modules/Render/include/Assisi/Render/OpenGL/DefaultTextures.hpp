#pragma once

#ifndef DEFAULTTEXTURES_HPP
#define DEFAULTTEXTURES_HPP

#include <glad/glad.h>

namespace Assisi::Render::OpenGL
{
class DefaultTextures
{
  public:
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
};
} /* namespace Assisi::Render::OpenGL */

#endif /* DEFAULTTEXTURES_HPP */