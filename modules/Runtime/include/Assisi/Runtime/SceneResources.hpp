/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneResources.hpp
/// @brief What the scene's passes hand one another during a frame.
///
/// Each is filled by the one pass that produces it and read by the passes
/// after it, and each is empty on a frame its producer does not run. A pass
/// that reads one binds what it holds itself, so a producer never reaches into
/// the passes that use its output.

#include <Assisi/Render/Clouds.hpp>
#include <Assisi/Render/LocalShadowPass.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Runtime/IndirectResolve.hpp>

#include <nvrhi/nvrhi.h>

#include <span>

namespace Assisi::Runtime
{

/// @brief The clouds' sky map, their shadow map and where the shadow lies.
struct CloudMap
{
    Render::CloudShadowConstants shadowConstants;
    nvrhi::ITexture *sky = nullptr;
    nvrhi::ITexture *shadow = nullptr;
};

/// @brief The sky probe's prefiltered cube, and whether it holds a bake.
struct SkyReflections
{
    nvrhi::ITexture *specularCube = nullptr;
    SpecularProbe probe;
};

/// @brief The sun's cascade array, and the fit the lit pass samples it with.
struct SunShadows
{
    Render::MeshPass::SunShadowFrame frame;
    nvrhi::ITexture *cascades = nullptr;
};

/// @brief The local-light atlas, and where each served light's views start.
struct LocalShadows
{
    /// One per light the atlas served; a light with none keeps no shadow.
    std::span<const Render::LocalShadowPass::Tile> tiles;
    nvrhi::ITexture *atlas = nullptr;
    Render::MeshPass::LocalShadowFrame frame;
};

/// @brief Screen-space occlusion's result.
struct ScreenOcclusion
{
    nvrhi::ITexture *texture = nullptr;
};

} // namespace Assisi::Runtime
