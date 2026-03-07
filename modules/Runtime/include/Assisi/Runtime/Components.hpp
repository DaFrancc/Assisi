#pragma once

/// @file Components.hpp
/// @brief ECS component types for rendering and world placement.
///
/// All types satisfy std::is_trivially_copyable so they can be stored in
/// SparseSet<T>. Use TransformComponent for position/rotation/scale and
/// MeshRendererComponent to associate a mesh and texture with an entity.

#include <Assisi/Prelude.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>

namespace Assisi::Runtime
{

/// @brief Local-space TRS with a cached world matrix updated by PropagateTransforms().
///
/// Write to position/rotation/scale to move an entity. Read worldMatrix for
/// rendering or any system that needs actual world-space coordinates.
/// worldMatrix is not serialized — it is recomputed every frame.
ACOMP()
struct TransformComponent
{
    AFIELD() glm::vec3 position{0.f, 0.f, 0.f};
    AFIELD() glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    AFIELD() glm::vec3 scale{1.f, 1.f, 1.f};

    glm::mat4 worldMatrix{1.f}; ///< Computed by PropagateTransforms(). Do not set manually.
};

/// @brief Associates a GPU mesh and PBR material textures with an entity.
///
/// `mesh` is a non-owning pointer — the MeshBuffer must outlive the component.
/// Each texture ID is an OpenGL texture object; 0 falls back to an appropriate
/// engine default:
///   - albedoTextureId   → 1×1 white texture
///   - normalTextureId   → 1×1 flat normal (0, 0, 1) in tangent space
///   - metallicTextureId → 1×1 black (metallic = 0, fully dielectric)
///   - roughnessTextureId → 1×1 mid-grey (roughness ≈ 0.5)
///
/// The pointer and texture IDs are runtime-only (transient); asset paths are
/// resolved by the scene loader and are not stored on the component itself.
ACOMP()
struct MeshRendererComponent
{
    AFIELD(transient) const Assisi::Render::OpenGL::MeshBuffer *mesh = nullptr;
    AFIELD(transient) unsigned int albedoTextureId   = 0u;
    AFIELD(transient) unsigned int normalTextureId   = 0u;
    AFIELD(transient) unsigned int metallicTextureId = 0u;
    AFIELD(transient) unsigned int roughnessTextureId = 0u;
};

/// @brief Projection and activation parameters for a camera entity.
///
/// Pair with TransformComponent to form a complete camera: the TransformComponent
/// provides world-space position and orientation; this component stores projection
/// settings and identifies which camera is active.
///
/// Call Runtime::ViewMatrix(transform) and Runtime::ProjectionMatrix(camera, aspect)
/// to obtain the matrices needed for rendering.
ACOMP()
struct CameraComponent
{
    AFIELD() float fovDegrees = 60.f;  ///< Vertical field of view in degrees.
    AFIELD() float nearZ      = 0.1f;  ///< Near clip plane distance.
    AFIELD() float farZ       = 200.f; ///< Far clip plane distance.
    AFIELD() bool  isActive   = false; ///< True for the scene's active camera.
};

} // namespace Assisi::Runtime