/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Components.hpp
/// @brief ECS component types for rendering and world placement.
///
/// Components are stored in SparseSet<T>, which holds them in a std::vector and
/// moves them on insert/remove — so any movable type works; trivial-copyability
/// is not required. Transform/Camera stay trivially copyable (plain data), but
/// MeshRenderer deliberately spends that property: its material lists are
/// std::vectors (variable slot count), so it is movable but not trivially
/// copyable.
/// Use Transform for position/rotation/scale and MeshRenderer
/// to associate a mesh and its materials with an entity.
///
/// Transform itself is defined one layer down in Assisi/ECS/Transform.hpp so
/// renderer-free modules (Physics, hierarchy) can use it without dragging in
/// nvrhi; it is re-exported below as Runtime::Transform for the render-facing
/// code that names it that way.

#include <vector>

#include <Assisi/Prelude.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Material.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Render/Texture.hpp>

namespace Assisi::Runtime
{

/// @brief The foundational TRS component, defined in the ECS layer.
/// Re-exported here so `Runtime::Transform` keeps naming it; see ECS/Transform.hpp.
using ECS::Transform;

/// @brief Associates a GPU mesh and its per-slot materials with an entity.
///
/// Two layers, by design:
///   - `mesh` / `materialOverrides` are the **durable** references — stable
///     GUIDs (`AssetId`) that persist in the level file. `mesh` selects the
///     geometry (a reserved built-in like `prim://cube`, or a mesh file's id);
///     nil → the unit cube. `materialOverrides` is a sparse, per-material-slot
///     list of `.amat` GUIDs: entry `i` overrides slot `i`; a nil or absent
///     entry means "use the material the mesh imported for that slot". Shorter
///     than the mesh's slot count is fine.
///   - `meshBuffer` / `materials` are **transient** non-owning pointers resolved
///     from those GUIDs by the asset cache at load time; the referenced GPU
///     resources must outlive the component. `materials` holds one resolved
///     Material per mesh slot (override or mesh default); a slot with no entry
///     draws with the cache's fallback material.
///
/// Replicable: a mirror with no mesh reference draws nothing. Only the durable
/// GUID layer travels — a resolved pointer is local to the process that owns it,
/// and every machine resolves its own from the same ids.
ACOMP(replicable)
struct MeshRenderer
{
    AFIELD() Assisi::Core::AssetId mesh;
    AFIELD() std::vector<Assisi::Core::AssetId> materialOverrides;

    /// Whether this mesh is drawn into shadow maps. Defaults ON: a piece of
    /// geometry that lights the room through itself is a bug the author opts
    /// into, not out of. Turning it off is the cheap fix for a caster whose
    /// shadow nobody sees — small props, interior detail, backfaces.
    ///
    /// Replicated with the mesh reference for the same reason: a spawned mirror
    /// that casts when the original does not is a visible desync.
    AFIELD() bool castsShadows = true;

    AFIELD(transient) const Assisi::Render::MeshBuffer *meshBuffer = nullptr;
    AFIELD(transient) std::vector<const Assisi::Render::Material *> materials;
};

/// @brief Projection and activation parameters for a camera entity.
///
/// Pair with Transform to form a complete camera: the Transform
/// provides world-space position and orientation; this component stores projection
/// settings and identifies which camera is active.
///
/// Call Runtime::ViewMatrix(transform) and Runtime::ProjectionMatrix(camera, aspect)
/// to obtain the matrices needed for rendering.
///
/// Deliberately **not** ACOMP(replicable), and the founding case for opt-in wire
/// gating: an arriving `isActive` would let the host hand a client a different
/// view than the one that client chose. Which camera a machine looks through is
/// that machine's business.
ACOMP()
struct Camera
{
    AFIELD() float fovDegrees = 60.f;  ///< Vertical field of view in degrees.
    AFIELD() float nearZ      = 0.1f;  ///< Near clip plane distance.
    AFIELD() float farZ       = 200.f; ///< Far clip plane distance.
    AFIELD() bool isActive   = false;  ///< True for the scene's active camera.
};

/// @brief Drives an entity's Transform back and forth along a fixed axis.
///
/// Pair with Transform, and run App::OscillateSystem in FixedUpdate. The pose is
/// **evaluated** from the fixed-step tick rather than integrated per frame:
/// position is `origin + axis * amplitude * sin(...)` every step, so it cannot
/// accumulate drift and two runs of the same level produce the same motion at
/// the same tick. That is what makes it usable in a measurement fixture, where a
/// mover that wandered between runs would put noise straight into the numbers.
///
/// `origin` is authored rather than captured on the first tick, for the same
/// reason: capturing would make the path depend on when the system first ran.
///
/// Not ACOMP(replicable) — every machine evaluates the same closed form from the
/// same tick and arrives at the same pose, so sending it would restate what both
/// ends already know.
ACOMP()
struct Oscillator
{
    AFIELD() glm::vec3 origin{0.f, 0.f, 0.f};  ///< World position the travel is centred on.
    AFIELD() glm::vec3 axis{0.f, 1.f, 0.f};    ///< Direction of travel; normalised on use, zero disables.
    AFIELD() float amplitude     = 1.f;         ///< Peak displacement from `origin`, in metres.
    AFIELD(min = 0) float periodSeconds = 4.f;  ///< Seconds for one full there-and-back cycle; 0 disables.
    AFIELD() float phase         = 0.f;         ///< Cycle offset in turns, so movers do not travel in lockstep.
};

} // namespace Assisi::Runtime