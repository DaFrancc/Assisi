/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/* Provide stb_image function bodies in exactly this translation unit. */
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/OpenGL/Texture2D.hpp>

namespace Assisi::Render::OpenGL
{

std::expected<void, Assisi::Core::AssetError> Texture2D::LoadFromAssets(std::string_view vpath) noexcept
{
    Destroy();

    auto resolved = Assisi::Core::AssetSystem::Resolve(vpath);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }

    stbi_set_flip_vertically_on_load(1);

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *data = stbi_load(resolved->string().c_str(), &width, &height, &channels, 4);
    if (data == nullptr)
    {
        Assisi::Core::Log::Error("Texture2D: stbi_load failed for '{}'", vpath);
        return std::unexpected(Assisi::Core::AssetError::FileReadFailed);
    }

    glGenTextures(1, &_textureId);
    glBindTexture(GL_TEXTURE_2D, _textureId);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(data);
    glBindTexture(GL_TEXTURE_2D, 0);

    return {};
}

} // namespace Assisi::Render::OpenGL