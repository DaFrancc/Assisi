#pragma once

/// @file MeshPass.hpp
/// @brief NVRHI graphics pipeline for drawing unlit, opaque scene geometry.
///
/// One pipeline shared by every entity: `MeshBuffer`'s vertex layout in, a
/// model-view-projection matrix pushed as a constant, a vertex+pixel shader
/// pair shading out. Materials, textures, and lighting are not wired up yet —
/// see docs/nvrhi-migration-todo.md.

#include <cstdint>
#include <string>

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

    /// @brief Records a draw of `mesh` with the given model-view-projection matrix.
    /// @pre IsValid() — call Initialize() first.
    void Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
              uint32_t viewportHeight, const glm::mat4 &modelViewProjection, const MeshBuffer &mesh) const;

    bool IsValid() const { return _pipeline != nullptr; }

  private:
    nvrhi::InputLayoutHandle      _inputLayout;
    nvrhi::BindingLayoutHandle    _bindingLayout;
    nvrhi::BindingSetHandle       _bindingSet;
    nvrhi::GraphicsPipelineHandle _pipeline;
};
} /* namespace Assisi::Render */
