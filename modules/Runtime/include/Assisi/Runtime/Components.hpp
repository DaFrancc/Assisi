/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Components.hpp
/// @brief ECS component types for rendering and world placement.
///
/// Components are stored in SparseSet<T>, which holds them in a std::vector and
/// moves them on insert/remove — so any movable type works; trivial-copyability
/// is not required. These particular components stay trivially copyable because
/// they are plain data (an AssetPath is a fixed inline buffer, not a heap
/// string), which keeps them cheap, but that is a property, not a constraint.
/// Use Transform for position/rotation/scale and MeshRenderer
/// to associate a mesh and texture with an entity.
///
/// Transform itself is defined one layer down in Assisi/ECS/Transform.hpp so
/// renderer-free modules (Physics, hierarchy) can use it without dragging in
/// nvrhi; it is re-exported below as Runtime::Transform for the render-facing
/// code that names it that way.

#include <Assisi/Prelude.hpp>
#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Runtime
{

/// @brief The foundational TRS component, defined in the ECS layer.
/// Re-exported here so `Runtime::Transform` keeps naming it; see ECS/Transform.hpp.
using ECS::Transform;

/// @brief Associates a GPU mesh and albedo texture with an entity.
///
/// Two layers, by design:
///   - `meshPath` / `albedoPath` are the **durable** references — virtual asset
///     paths (e.g. "prim://cube", "textures/crate.png") that persist in the
///     level file. An empty path selects the engine default (unit cube / flat
///     white).
///   - `mesh` / `albedoTexture` are **transient** non-owning pointers resolved
///     from those paths by the asset cache at load time; the referenced GPU
///     resources must outlive the component. A null `albedoTexture` falls back
///     to a flat white default (see Render::DefaultResources::WhiteTexture).
///
/// Normal/metallic/roughness maps aren't wired up yet — see
/// docs/nvrhi-migration-todo.md.
ACOMP()
struct MeshRenderer
{
    AFIELD() Assisi::Core::AssetPath meshPath;
    AFIELD() Assisi::Core::AssetPath albedoPath;

    AFIELD(transient) const Assisi::Render::MeshBuffer *mesh = nullptr;
    AFIELD(transient) const Assisi::Render::Texture *albedoTexture = nullptr;
};

/// @brief Projection and activation parameters for a camera entity.
///
/// Pair with Transform to form a complete camera: the Transform
/// provides world-space position and orientation; this component stores projection
/// settings and identifies which camera is active.
///
/// Call Runtime::ViewMatrix(transform) and Runtime::ProjectionMatrix(camera, aspect)
/// to obtain the matrices needed for rendering.
ACOMP()
struct Camera
{
    AFIELD() float fovDegrees = 60.f;  ///< Vertical field of view in degrees.
    AFIELD() float nearZ      = 0.1f;  ///< Near clip plane distance.
    AFIELD() float farZ       = 200.f; ///< Far clip plane distance.
    AFIELD() bool  isActive   = false; ///< True for the scene's active camera.
};

} // namespace Assisi::Runtime