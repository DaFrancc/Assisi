/*
 * Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")
 */

#include <Assisi/Render/DefaultResources.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Render
{
nvrhi::ITexture *DefaultResources::WhiteTexture(nvrhi::IDevice *device)
{
    static Texture texture;
    if (!texture.IsValid())
    {
        // sRGB to match the scene albedo textures it stands in for; white is the
        // same value either way, but it keeps the default consistent with real maps.
        texture.UploadSolidColor(device, 255, 255, 255, 255, ColorSpace::Srgb, "DefaultResources::White");
    }
    return texture.NativeTexture();
}
} /* namespace Assisi::Render */
