/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshPass.hpp
/// @brief NVRHI graphics pipeline for drawing lit, opaque scene geometry.
///
/// One pipeline shared by every entity: `MeshBuffer`'s vertex layout in, a
/// per-draw model + model-view-projection matrix pushed as constants, a
/// per-frame constant buffer carrying the camera and cluster-grid parameters,
/// a full glTF metallic-roughness Material (base colour, normal, metallic-
/// roughness, occlusion, emissive — factors + textures) bound per-submesh, and
/// clustered point/spot/directional lighting (see ClusterGrid) read directly
/// from cube_min.frag.

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ClusterGrid.hpp>
#include <Assisi/Render/Material.hpp>
#include <Assisi/Render/MeshBuffer.hpp>

namespace Assisi::Render
{
class MeshPass
{
  public:
    MeshPass() = default;

    /// @brief Loads `vertexShaderSpvPath`/`pixelShaderSpvPath` (compiled SPIR-V,
    /// see ShaderModule.hpp) and builds the pipeline against the given
    /// framebuffer format. `clusterGrid` must outlive this MeshPass — its
    /// light buffers are bound directly into every binding set this pass creates.
    /// @return false if either shader failed to load or the pipeline failed to build.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo,
                                  const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath,
                                  const ClusterGrid &clusterGrid);

    /// @brief Recreates just the graphics pipeline against a new FramebufferInfo,
    /// reusing the shaders/input layout/binding layout already loaded by
    /// Initialize(). Needed when the render target's sample count changes at
    /// runtime (see Render::PostProcess) — a pipeline built for sampleCount=1
    /// isn't compatible with an MSAA framebuffer. Existing binding sets (which
    /// don't depend on FramebufferInfo) stay valid and cached.
    /// @pre IsValid() — call Initialize() first.
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Updates the per-frame constant buffer (camera view matrix and
    /// cluster-grid parameters). Call once per frame, before any Draw() calls.
    void UpdateFrameConstants(nvrhi::ICommandList *commandList, const glm::mat4 &view, uint32_t screenWidth,
                              uint32_t screenHeight, float nearZ, float farZ, uint32_t dirLightCount) const;

    /// @brief Records the draws for `mesh`'s LOD0 submeshes with the given model
    /// and model-view-projection matrices, binding the Material for each submesh's
    /// slot and lighting the surface from ClusterGrid's buffers.
    /// @param materials  One Material per mesh material slot (see
    /// MeshRenderer::materials). A submesh whose slot has no entry is skipped;
    /// the pointers must stay valid until this draw is submitted.
    /// @pre IsValid() — call Initialize() first, and UpdateFrameConstants() this frame.
    void Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
              uint32_t viewportHeight, const glm::mat4 &modelViewProjection, const glm::mat4 &model,
              const MeshBuffer &mesh, std::span<const Material *const> materials) const;

    bool IsValid() const { return _pipeline != nullptr; }

    /// @brief Drops every cached material binding set. Call when the resources they
    /// reference may be freed or replaced (e.g. an AssetCache is cleared or a
    /// level is unloaded) so a later Draw() rebuilds against live resources
    /// instead of returning a binding set that pins a destroyed texture — or,
    /// worse, aliases a freed address the allocator has since reused.
    void InvalidateBindingSets() { _bindingSetCache.clear(); }

  private:
    /// @brief Returns the binding set for `material`, creating and caching it on
    /// first use, keyed by the material's stable id. NVRHI binding sets reference
    /// concrete resources (the material's constants buffer + five textures), so
    /// one is needed per distinct material — unlike the mesh, which is bound
    /// directly via vertex/index buffer bindings rather than the binding set.
    nvrhi::IBindingSet *GetOrCreateBindingSet(const Material &material) const;

    nvrhi::IDevice *_device = nullptr;
    const ClusterGrid *_clusterGrid = nullptr;

    nvrhi::ShaderHandle           _vertexShader;
    nvrhi::ShaderHandle           _pixelShader;
    nvrhi::InputLayoutHandle      _inputLayout;
    nvrhi::BindingLayoutHandle    _bindingLayout;
    nvrhi::SamplerHandle          _sampler;
    nvrhi::GraphicsPipelineHandle _pipeline;
    nvrhi::BufferHandle           _frameConstantsBuffer;

    /// @brief Cache of binding sets keyed by the material's stable id (Material::Id).
    ///
    /// The id is monotonic and never reused (it survives AssetCache::Clear), so a
    /// stale entry is dead, never wrong: an id whose material was freed simply
    /// never comes up again. When the asset set backing the scene changes (a level
    /// unload or an AssetCache clear frees the materials' GPU resources), the owner
    /// still calls InvalidateBindingSets() for memory hygiene — otherwise dead
    /// entries would pin their constants buffers and textures alive forever. A
    /// future streaming path evicts per-id rather than clearing wholesale.
    mutable std::unordered_map<uint32_t, nvrhi::BindingSetHandle> _bindingSetCache;
};
} /* namespace Assisi::Render */
