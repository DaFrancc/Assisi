#pragma once

/// @file MeshPass.hpp
/// @brief NVRHI graphics pipeline for drawing unlit, opaque scene geometry.
///
/// One pipeline shared by every entity: `MeshBuffer`'s vertex layout in, a
/// model-view-projection matrix pushed as a constant, an albedo texture
/// sampled per-entity, a vertex+pixel shader pair shading out. Normal maps,
/// metallic/roughness, and lighting are not wired up yet — see
/// docs/nvrhi-migration-todo.md.

#include <cstdint>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/MeshBuffer.hpp>

namespace Assisi::Render
{
class MeshPass
{
  public:
    MeshPass() = default;

    /// @brief Loads `vertexShaderSpvPath`/`pixelShaderSpvPath` (compiled SPIR-V,
    /// see ShaderModule.hpp) and builds the pipeline against the given
    /// framebuffer format.
    /// @return false if either shader failed to load or the pipeline failed to build.
    bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo,
                    const std::string &vertexShaderSpvPath, const std::string &pixelShaderSpvPath);

    /// @brief Records a draw of `mesh` with the given model-view-projection matrix,
    /// sampling `albedoTexture` in the pixel shader.
    /// @param albedoTexture  Pass nullptr to use a flat white fallback
    /// (Render::DefaultResources::WhiteTexture).
    /// @pre IsValid() — call Initialize() first.
    void Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
              uint32_t viewportHeight, const glm::mat4 &modelViewProjection, const MeshBuffer &mesh,
              nvrhi::ITexture *albedoTexture = nullptr) const;

    bool IsValid() const { return _pipeline != nullptr; }

  private:
    /// @brief Returns the binding set for `albedoTexture`, creating and caching it
    /// on first use. NVRHI binding sets reference concrete resources, so one is
    /// needed per distinct texture — unlike the mesh, which is bound directly via
    /// vertex/index buffer bindings rather than through the binding set.
    nvrhi::IBindingSet *GetOrCreateBindingSet(nvrhi::ITexture *albedoTexture) const;

    nvrhi::IDevice *_device = nullptr;

    nvrhi::InputLayoutHandle      _inputLayout;
    nvrhi::BindingLayoutHandle    _bindingLayout;
    nvrhi::SamplerHandle          _sampler;
    nvrhi::GraphicsPipelineHandle _pipeline;

    mutable std::unordered_map<nvrhi::ITexture *, nvrhi::BindingSetHandle> _bindingSetCache;
};
} /* namespace Assisi::Render */
