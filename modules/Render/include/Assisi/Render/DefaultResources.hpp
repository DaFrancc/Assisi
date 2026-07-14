/*
 * Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc")
 */

#pragma once

/// @file DefaultResources.hpp
/// @brief Engine-wide fallback GPU resources.
///
/// Provides access to shared default textures (e.g. the 1x1 white texture)
/// used when no explicit resource has been assigned to an object.

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{
/// @brief Provides access to built-in fallback GPU resources.
///
/// One 1x1 texture per material channel, so a Material with an empty channel
/// samples a neutral default and the shader never branches on a null texture:
///   - White sRGB    — baseColor (and emissive, though emissive factor is 0).
///   - White linear  — metallic-roughness / occlusion: sampling 1.0 leaves the
///                     per-material factor untouched (glTF "no texture" semantics).
///   - Flat normal   — (128,128,255) linear = the +Z tangent-space normal, i.e.
///                     "no perturbation".
/// All are created lazily on first request and shared process-wide.
class DefaultResources
{
  public:
    /// @brief Engine-wide 1x1 white sRGB texture (albedo fallback).
    static nvrhi::ITexture *WhiteTexture(nvrhi::IDevice *device);

    /// @brief 1x1 white *linear* texture (metallic-roughness / occlusion fallback).
    static nvrhi::ITexture *WhiteLinearTexture(nvrhi::IDevice *device);

    /// @brief 1x1 flat tangent-space normal (128,128,255) linear texture.
    static nvrhi::ITexture *FlatNormalTexture(nvrhi::IDevice *device);
};
} /* namespace Assisi::Render */
