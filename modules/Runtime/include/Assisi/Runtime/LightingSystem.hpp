/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file LightingSystem.hpp
/// @brief Bridges the ECS scene to the clustered forward lighting pipeline.
///
/// LightingSystem queries light components from the scene each frame,
/// uploads them to the GPU, and runs the cluster culling compute pass.
///
/// Usage:
///   1. Call Initialize() once after the NVRHI device is ready.
///   2. Call Resize() whenever the viewport or projection changes.
///   3. Call Update() every frame before DrawScene().

#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ClusterGrid.hpp>

#include <cstdint>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace Assisi::Runtime
{

class LightingSystem
{
public:
    LightingSystem() = default;

    /// @brief Load compute shaders, allocate buffers, and build initial cluster AABBs.
    /// @param width,height  Framebuffer size in pixels.
    /// @param nearZ,farZ    Near/far clip distances matching the projection matrix.
    /// @param projection    The camera projection matrix.
    /// @return false if compute shaders failed to compile.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, int32_t width,
                                  int32_t height, float nearZ, float farZ, const glm::mat4 &projection);

    /// @brief Rebuild cluster AABBs after a viewport or projection change.
    void Resize(nvrhi::ICommandList *commandList, int32_t width, int32_t height, float nearZ, float farZ,
                const glm::mat4 &projection);

    /// @brief Collect lights from the scene, upload to GPU, and run the cull pass.
    void Update(nvrhi::ICommandList *commandList, Assisi::ECS::Scene &scene, const glm::mat4 &view);

    /// @brief A spot light's aim in world space: its LOCAL direction rotated by the
    /// entity's propagated world matrix, normalized.
    ///
    /// Split out of Update so the rule is testable without a device (Update needs a
    /// command list). A spot mounted on a parent — a vehicle headlight, a held torch
    /// — must aim where the parent faces, as its position already does.
    ///
    /// A direction is a vector rather than a normal, so the upper-left 3x3 is the
    /// right transform — no inverse-transpose. Normalizing afterwards absorbs any
    /// scale, and falls back to a fixed axis for a degenerate (zero) direction
    /// instead of producing NaN.
    [[nodiscard]] static glm::vec3 WorldSpotDirection(const glm::mat4 &worldMatrix, const glm::vec3 &localDirection);

    /// @brief Number of directional lights found in the last Update() call.
    uint32_t DirLightCount() const { return _dirLightCount; }

    const Assisi::Render::ClusterGrid &Grid() const { return _grid; }

private:
    Assisi::Render::ClusterGrid _grid;
    uint32_t _dirLightCount = 0u;

    // Per-frame light staging buffers, kept as members so Update() reuses their
    // capacity instead of allocating three vectors every frame (clear() retains
    // storage). Repopulated from scratch each Update(); not valid between calls.
    std::vector<Assisi::Render::PointLightGPU> _pointLights;
    std::vector<Assisi::Render::SpotLightGPU>  _spotLights;
    std::vector<Assisi::Render::DirLightGPU>   _dirLights;
};

} // namespace Assisi::Runtime
