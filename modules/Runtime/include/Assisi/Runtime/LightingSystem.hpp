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
#include <Assisi/Render/Sky.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace Assisi::Runtime
{

/// @brief One light's `castsShadows`, as the gathered light arrays carry it.
///
/// A named enum rather than a bare byte so a reader of `std::span<const
/// ShadowCaster>` cannot mistake it for a count, an index, or a bitmask — and
/// `uint8_t` rather than `bool` because std::vector<bool> is a bitset whose
/// elements have no address, so no span can point at one.
enum class ShadowCaster : uint8_t
{
    No  = 0,
    Yes = 1,
};

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

    /// @brief The colour a directional light contributes, after the atmosphere on
    /// its own entity has had it.
    ///
    /// Split out of Update for the same reason WorldSpotDirection is: the rule is
    /// the feature, and Update needs a command list.
    ///
    /// @p atmosphere is null under SunColorSource::Authored, or when the entity
    /// carries no Skybox — and then @p color passes through untouched. Otherwise
    /// @p color is NOT read: the sky supplies the colour outright, which is what
    /// lets the inspector grey the field rather than leave it looking live.
    ///
    /// Extinction dims as well as tints, so both ride here and the light's
    /// authored intensity is left exactly as authored.
    [[nodiscard]] static glm::vec3 SunlightColor(const glm::vec3 &color, const glm::vec3 &directionToSun,
                                                 const Render::SkySettings *atmosphere);

    /// @brief Number of directional lights found in the last Update() call.
    uint32_t DirLightCount() const { return _dirLightCount; }

    /// @brief The directional light the sun's cascades are rendered for.
    struct ShadowSun
    {
        /// Index into the directional-light buffer the shader reads, so both
        /// halves name the same light without either re-deriving the order.
        uint32_t index = 0;
        /// The direction the light travels, normalised.
        glm::vec3 direction{0.f, -1.f, 0.f};
    };

    /// @brief The first shadow-casting directional light of the last Update(),
    /// or nothing when none of them casts.
    ///
    /// One sun, not all of them: a second shadowed directional light would
    /// double the cascade array and every depth draw in the frame, and a scene
    /// with two suns in it is not one this engine has been asked for. The rest
    /// still light; they just do not occlude.
    [[nodiscard]] std::optional<ShadowSun> ShadowCastingSun() const;

    /// @name Shadow-casting flags from the last Update()
    ///
    /// One entry per light, in the same order as the buffers uploaded to the GPU
    /// — element `i` is the light the shaders see at index `i` of its type. That
    /// parallelism is the point: a shadow pass picks which lights get a map and
    /// hands the shader a slot per light index, so both halves must agree on what
    /// index `i` means without re-querying the scene and re-deriving the order.
    ///
    /// Kept beside the GPU structs rather than inside them because no shader
    /// reads a flag: the sun's shadow reaches the mesh shader as the single
    /// light index ShadowCastingSun() reports, and one index costs less than a
    /// byte on every light in the buffer.
    ///
    /// Valid until the next Update().
    /// @{
    [[nodiscard]] std::span<const ShadowCaster> PointLightShadowFlags() const { return _pointShadowFlags; }
    [[nodiscard]] std::span<const ShadowCaster> SpotLightShadowFlags() const { return _spotShadowFlags; }
    /// Truncated to DirLightCount(), matching the directional buffer the shader reads.
    [[nodiscard]] std::span<const ShadowCaster> DirLightShadowFlags() const
    {
        return std::span<const ShadowCaster>(_dirShadowFlags).first(_dirLightCount);
    }
    /// @}

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

    // Index-parallel to the three above; same reuse, same lifetime.
    std::vector<ShadowCaster> _pointShadowFlags;
    std::vector<ShadowCaster> _spotShadowFlags;
    std::vector<ShadowCaster> _dirShadowFlags;
};

} // namespace Assisi::Runtime
