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

nvrhi::ITexture *DefaultResources::WhiteLinearTexture(nvrhi::IDevice *device)
{
    static Texture texture;
    if (!texture.IsValid())
    {
        // Linear: metallic-roughness and occlusion are data maps, not colour.
        // Sampling 1.0 means the material's metallic/roughness/occlusion factors
        // pass through unmodified — the glTF "channel has no texture" behaviour.
        texture.UploadSolidColor(device, 255, 255, 255, 255, ColorSpace::Linear, "DefaultResources::WhiteLinear");
    }
    return texture.NativeTexture();
}

nvrhi::ITexture *DefaultResources::FlatNormalTexture(nvrhi::IDevice *device)
{
    static Texture texture;
    if (!texture.IsValid())
    {
        // (128,128,255) decodes (x*2-1) to ~(0,0,1): the unperturbed tangent-space
        // normal. Linear, since a normal map holds vector data, not colour.
        texture.UploadSolidColor(device, 128, 128, 255, 255, ColorSpace::Linear, "DefaultResources::FlatNormal");
    }
    return texture.NativeTexture();
}
} /* namespace Assisi::Render */
