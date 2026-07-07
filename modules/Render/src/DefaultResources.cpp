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
        texture.UploadSolidColor(device, 255, 255, 255, 255, "DefaultResources::White");
    }
    return texture.NativeTexture();
}
} /* namespace Assisi::Render */
