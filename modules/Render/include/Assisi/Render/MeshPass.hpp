#pragma once

/// @file MeshPass.hpp
/// @brief NVRHI graphics pipeline for drawing lit, opaque scene geometry.
///
/// One pipeline shared by every entity: `MeshBuffer`'s vertex layout in, a
/// per-draw model + model-view-projection matrix pushed as constants, a
/// per-frame constant buffer carrying the camera and cluster-grid parameters,
/// an albedo texture sampled per-entity, and clustered point/spot/directional
/// lighting (see ClusterGrid) read directly from cube_min.frag. Normal maps
/// and metallic/roughness maps are not wired up yet — see
/// docs/nvrhi-migration-todo.md.

#include <cstdint>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ClusterGrid.hpp>
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
    bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo,
                    const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath,
                    const ClusterGrid &clusterGrid);

    /// @brief Updates the per-frame constant buffer (camera view matrix and
    /// cluster-grid parameters). Call once per frame, before any Draw() calls.
    void UpdateFrameConstants(nvrhi::ICommandList *commandList, const glm::mat4 &view, uint32_t screenWidth,
                              uint32_t screenHeight, float nearZ, float farZ, uint32_t dirLightCount) const;

    /// @brief Records a draw of `mesh` with the given model and
    /// model-view-projection matrices, sampling `albedoTexture` in the pixel
    /// shader and lighting the surface from ClusterGrid's buffers.
    /// @param albedoTexture  Pass nullptr to use a flat white fallback
    /// (Render::DefaultResources::WhiteTexture).
    /// @pre IsValid() — call Initialize() first, and UpdateFrameConstants() this frame.
    void Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
              uint32_t viewportHeight, const glm::mat4 &modelViewProjection, const glm::mat4 &model,
              const MeshBuffer &mesh, nvrhi::ITexture *albedoTexture = nullptr) const;

    bool IsValid() const { return _pipeline != nullptr; }

  private:
    /// @brief Returns the binding set for `albedoTexture`, creating and caching it
    /// on first use. NVRHI binding sets reference concrete resources, so one is
    /// needed per distinct texture — unlike the mesh, which is bound directly via
    /// vertex/index buffer bindings rather than through the binding set.
    nvrhi::IBindingSet *GetOrCreateBindingSet(nvrhi::ITexture *albedoTexture) const;

    nvrhi::IDevice *_device = nullptr;
    const ClusterGrid *_clusterGrid = nullptr;

    nvrhi::InputLayoutHandle      _inputLayout;
    nvrhi::BindingLayoutHandle    _bindingLayout;
    nvrhi::SamplerHandle          _sampler;
    nvrhi::GraphicsPipelineHandle _pipeline;
    nvrhi::BufferHandle           _frameConstantsBuffer;

    mutable std::unordered_map<nvrhi::ITexture *, nvrhi::BindingSetHandle> _bindingSetCache;
};
} /* namespace Assisi::Render */
