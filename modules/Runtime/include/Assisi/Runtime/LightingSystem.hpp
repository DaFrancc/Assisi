/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LightingSystem.hpp
/// @brief Bridges the ECS scene to the clustered forward lighting pipeline.
///
/// LightingSystem queries light components from the scene each frame,
/// uploads them to the GPU, and runs the cluster culling compute pass.
///
/// Usage:
///   1. Call Initialize() once after the OpenGL context is ready.
///   2. Call Resize() whenever the viewport or projection changes.
///   3. Call Update() every frame before DrawScene().
///   4. After Update(), set `uDirLightCount` on the mesh shader using DirLightCount().

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ClusterGrid.hpp>
#include <Assisi/Render/Shader.hpp>

#include <cstdint>

namespace Assisi::Runtime
{

class LightingSystem
{
  public:
    LightingSystem() = default;

    /// @brief Load compute shaders, allocate SSBOs, and build initial cluster AABBs.
    /// @param width,height  Framebuffer size in pixels.
    /// @param nearZ,farZ    Near/far clip distances matching the projection matrix.
    /// @param projection    The camera projection matrix.
    /// @return false if compute shaders failed to compile.
    bool Initialize(int width, int height, float nearZ, float farZ, const glm::mat4 &projection);

    /// @brief Rebuild cluster AABBs after a viewport or projection change.
    void Resize(int width, int height, float nearZ, float farZ, const glm::mat4 &projection);

    /// @brief Collect lights from the scene, upload to GPU, and run the cull pass.
    ///        Binds all SSBOs to their fixed binding points for the mesh shader.
    void Update(Assisi::ECS::Scene &scene, const glm::mat4 &view);

    /// @brief Set the static cluster uniforms on the mesh shader.
    ///        Call once after Initialize() and again after every Resize().
    void SetupMeshShader(Assisi::Render::Shader &shader) const;

    /// @brief Number of directional lights found in the last Update() call.
    ///        Set as `uDirLightCount` on the mesh shader each frame.
    uint32_t DirLightCount() const { return _dirLightCount; }

    const Assisi::Render::ClusterGrid &Grid() const { return _grid; }

  private:
    Assisi::Render::ClusterGrid _grid;
    uint32_t                    _dirLightCount = 0u;
};

} // namespace Assisi::Runtime