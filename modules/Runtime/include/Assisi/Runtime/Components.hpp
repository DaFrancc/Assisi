#pragma once

/// @file Components.hpp
/// @brief ECS component types for rendering and world placement.
///
/// All types satisfy std::is_trivially_copyable so they can be stored in
/// SparseSet<T>. Use TransformComponent for position/rotation/scale and
/// MeshRendererComponent to associate a mesh and texture with an entity.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/OpenGL/MeshBuffer.hpp>

namespace Assisi::Runtime
{

/// @brief World-space TRS stored as plain data — required to be trivially copyable.
struct TransformComponent
{
    glm::vec3 position{0.f, 0.f, 0.f};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f}; ///< Identity quaternion.
    glm::vec3 scale{1.f, 1.f, 1.f};
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
struct MeshRendererComponent
{
    const Assisi::Render::OpenGL::MeshBuffer *mesh = nullptr;
    unsigned int albedoTextureId   = 0u;
    unsigned int normalTextureId   = 0u;
    unsigned int metallicTextureId = 0u;
    unsigned int roughnessTextureId = 0u;
};

} // namespace Assisi::Runtime