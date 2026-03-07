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

/// @brief World-space TRS stored as plain data — required to be trivially copyable.
ACOMP()
struct TransformComponent
{
    AFIELD() glm::vec3 position{0.f, 0.f, 0.f};
    AFIELD() glm::quat rotation{1.f, 0.f, 0.f, 0.f}; ///< Identity quaternion.
    AFIELD() glm::vec3 scale{1.f, 1.f, 1.f};
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
struct CameraComponent
{
    float fovDegrees = 60.f;  ///< Vertical field of view in degrees.
    float nearZ      = 0.1f;  ///< Near clip plane distance.
    float farZ       = 200.f; ///< Far clip plane distance.
    bool  isActive   = false; ///< True for the scene's active camera.
};

} // namespace Assisi::Runtime