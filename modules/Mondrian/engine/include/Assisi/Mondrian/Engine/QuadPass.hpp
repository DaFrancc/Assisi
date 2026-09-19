/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file QuadPass.hpp
/// @brief Draws a finalized Mondrian draw list into a display-encoded target.
///
/// One pipeline and one shader for every quad: shape, border and clip are
/// signed distances in the fragment shader, and each draw entry is one
/// instanced draw. Colours are written as authored, with no tone map after
/// them, and blended premultiplied.

#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Render/Texture.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

namespace Assisi::Mondrian::Engine
{

class QuadPass
{
public:
    /// @brief Builds the pipeline against @p framebufferInfo, which every
    /// framebuffer later handed to Draw must match, and the white texture every
    /// untextured draw samples. False, logged, when any of it cannot be built.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Makes @p texture drawable by the UI and returns its id. The pass
    /// holds a reference until Shutdown.
    ///
    /// The UI blends in display space, so a texture must hold display values
    /// in a non-sRGB format: an sRGB one is decoded to linear when sampled and
    /// draws too dark.
    [[nodiscard]] TextureId RegisterTexture(nvrhi::ITexture *texture);

    /// @brief Records @p drawList into @p framebuffer. A no-op before a
    /// successful Initialize and for an empty list.
    ///
    /// Rewrites one instance buffer, so it may run once per command list: a
    /// second Draw on the same list would overwrite instances the first has not
    /// drawn yet.
    void Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, const DrawList &drawList);

    /// @brief Releases every GPU handle. Must run before the device is destroyed.
    void Shutdown();

private:
    /// Descriptor sets, in the order the shader numbers them.
    enum class BindingSet : uint32_t
    {
        Instances,
        Texture,
        Count
    };

    [[nodiscard]] bool BuildLayouts();
    [[nodiscard]] bool BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);

    /// Grows the instance buffer to hold at least @p instanceCount, rebuilding
    /// the one binding set that names it. False on a failed allocation.
    [[nodiscard]] bool EnsureInstanceCapacity(uint32_t instanceCount);

    nvrhi::IDevice *_device = nullptr;
    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::BindingLayoutHandle _instanceLayout;
    nvrhi::BindingLayoutHandle _textureLayout;
    nvrhi::GraphicsPipelineHandle _pipeline;
    nvrhi::SamplerHandle _sampler;

    nvrhi::BufferHandle _instanceBuffer;
    nvrhi::BindingSetHandle _instanceSet;

    /// One binding set per registered texture, indexed by TextureId::value.
    std::vector<nvrhi::BindingSetHandle> _textureSets;

    /// What kWhiteTexture draws with.
    Render::Texture _white;

    uint32_t _instanceCapacity = 0;

    /// Whether the target encodes sRGB on write, so the shader decodes display
    /// colours first and the encode restores them.
    bool _encodeSrgb = false;
};

/// @brief Uploads the checkerboard the placeholder strip's textured tile shows.
/// Display values in a non-sRGB format, as RegisterTexture requires.
void UploadPlaceholderTexture(Render::Texture &texture, nvrhi::IDevice *device);

} // namespace Assisi::Mondrian::Engine
