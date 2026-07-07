#pragma once

/// @file ComputeShader.hpp
/// @brief NVRHI compute pipeline wrapper: one compiled SPIR-V compute shader
/// plus the binding layout it dispatches against.
///
/// The binding *layout* (what kinds of resources, at which slots) is fixed at
/// Initialize() time; the binding *set* (which concrete buffers) is supplied
/// by the caller per dispatch, since a single compute shader here is reused
/// across cluster-build/cull calls whose bound buffers don't change but whose
/// push constants do every frame.

#include <cstddef>
#include <cstdint>
#include <string>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{

/// @brief Owns a linked NVRHI compute pipeline built from a single SPIR-V shader.
class ComputeShader
{
  public:
    ComputeShader() = default;

    /// @brief Loads `spvPath` (compiled SPIR-V, see ShaderModule.hpp) and builds
    /// a compute pipeline against `bindingLayoutDesc`.
    /// @return false if the shader failed to load or the pipeline failed to build.
    bool Initialize(nvrhi::IDevice *device, const std::string &spvPath,
                    const nvrhi::BindingLayoutDesc &bindingLayoutDesc);

    /// @brief Records a dispatch with `bindingSet` bound and, if `pushConstants`
    /// is non-null, `pushConstantSize` bytes pushed before dispatching.
    /// @pre IsValid() — call Initialize() first.
    void Dispatch(nvrhi::ICommandList *commandList, nvrhi::IBindingSet *bindingSet, uint32_t groupsX,
                 uint32_t groupsY, uint32_t groupsZ, const void *pushConstants = nullptr,
                 size_t pushConstantSize = 0) const;

    bool IsValid() const { return _pipeline != nullptr; }
    nvrhi::IBindingLayout *BindingLayout() const { return _bindingLayout; }

  private:
    nvrhi::BindingLayoutHandle _bindingLayout;
    nvrhi::ComputePipelineHandle _pipeline;
};

} // namespace Assisi::Render
