/*
 * Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")
 */

#include <Assisi/Render/DefaultResources.hpp>
#include <Assisi/Render/OpenGL/DefaultTextures.hpp>

namespace Assisi::Render
{
unsigned int DefaultResources::WhiteTextureId()
{
    return Assisi::Render::OpenGL::DefaultTextures::WhiteTexture();
}
} /* namespace Assisi::Render */
