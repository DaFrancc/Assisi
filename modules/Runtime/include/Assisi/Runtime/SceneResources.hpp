/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SceneResources.hpp
/// @brief What the scene's passes hand one another during a frame.
///
/// Each is filled by the one pass that produces it and read by the passes
/// after it, and each is empty on a frame its producer does not run. A pass
/// that reads one binds what it holds itself, so a producer never reaches into
/// the passes that use its output. Some hold nothing at all: they stand for
/// work one pass leaves where the next finds it, such as vertices posed in the
/// shared geometry, so that what reads it is ordered after what writes it.

#include <Assisi/Render/Clouds.hpp>
#include <Assisi/Render/DrawItem.hpp>
#include <Assisi/Render/FramePass.hpp>
#include <Assisi/Render/LocalShadowPass.hpp>
#include <Assisi/Render/MeshPass.hpp>
#include <Assisi/Runtime/IndirectResolve.hpp>

#include <nvrhi/nvrhi.h>

#include <span>
#include <vector>

namespace Assisi::Runtime
{

/// @brief The clouds' sky map, their shadow map and where the shadow lies.
struct CloudMap
{
    Render::CloudShadowConstants shadowConstants;
    /// How far the clouds have drifted, which the probe's bake places them by.
    glm::dvec2 drift{0.0};
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

/// @brief Screen-space occlusion's texture, decided before the passes run so
/// the frame constants can say whether the lit pass reads it; null while it
/// is off.
struct ScreenOcclusion
{
    nvrhi::ITexture *texture = nullptr;
};

/// @brief Skinned meshes' vertices, posed for this frame in the shared geometry.
struct PosedVertices
{
};

/// @brief The mesh pass's constants for this frame.
struct FrameConstants
{
};

/// @brief The prepass's draws, which the lit pass replays.
struct OpaqueDraws
{
};

/// @brief The see-through surfaces in view, farthest first. Kept across frames
/// so a frame allocates nothing.
struct BlendedDraws
{
    std::vector<Render::DrawItem> items;
};

/// @brief The prepass's depth as a distance per pixel.
struct SceneDistance
{
};

/// @brief Screen-space occlusion's result, in ScreenOcclusion's texture.
struct Occlusion
{
};

/// @brief The scene's colour: what the lit pass, the sky and the see-through
/// surfaces draw into.
struct SceneColor
{
    Render::RenderTarget target;
};

/// @brief The scene's depth: what the prepass writes and the passes after it
/// test against.
struct SceneDepth
{
    Render::RenderTarget target;
};

} // namespace Assisi::Runtime
