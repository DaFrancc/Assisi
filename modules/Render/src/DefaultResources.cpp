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

unsigned int DefaultResources::FlatNormalTextureId()
{
    return Assisi::Render::OpenGL::DefaultTextures::FlatNormalTexture();
}

unsigned int DefaultResources::BlackTextureId()
{
    return Assisi::Render::OpenGL::DefaultTextures::BlackTexture();
}

unsigned int DefaultResources::GreyTextureId()
{
    return Assisi::Render::OpenGL::DefaultTextures::GreyTexture();
}
} /* namespace Assisi::Render */
