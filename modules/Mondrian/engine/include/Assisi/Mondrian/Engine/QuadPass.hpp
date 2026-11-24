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
#include <Assisi/Mondrian/Font.hpp>
#include <Assisi/Render/Texture.hpp>

#include <Assisi/Core/AssetId.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace Assisi::Mondrian::Engine
{

class QuadPass
{
public:
    /// @brief Builds the pipeline against @p framebufferInfo, the window's, and
    /// the white texture every untextured draw samples. False, logged, when any
    /// of it cannot be built. A framebuffer of another format, such as a
    /// picture shown on a surface in the world, gets a pipeline of its own the
    /// first time it is drawn into.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo);

    /// @brief Starts a frame's draws over: each Draw after it writes its quads
    /// after the last one's, so several may share a command list.
    void BeginFrame() { _instanceCursor = 0; }

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
    /// Its quads go after those of every Draw since BeginFrame, in one instance
    /// buffer, so no Draw overwrites quads an earlier one has not drawn yet.
    void Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, const DrawList &drawList);

    /// @brief Releases every GPU handle. Must run before the device is destroyed.
    void Shutdown();

private:
    /// Descriptor sets, in the order the shader numbers them.
    enum class BindingSet : uint32_t
    {
        Instances,
        Texture,
        Count_
    };

    /// One pipeline, and the format of the target it draws into.
    struct Pipeline
    {
        nvrhi::GraphicsPipelineHandle pipeline;
        nvrhi::Format format = nvrhi::Format::UNKNOWN;
    };

    [[nodiscard]] bool BuildLayouts();
    /// The pipeline for @p framebufferInfo's format, built the first time it is
    /// asked for; null, logged, when it cannot be built.
    [[nodiscard]] nvrhi::IGraphicsPipeline *PipelineFor(const nvrhi::FramebufferInfo &framebufferInfo);

    /// Grows the instance buffer to hold at least @p instanceCount, rebuilding
    /// the one binding set that names it. False on a failed allocation.
    [[nodiscard]] bool EnsureInstanceCapacity(uint32_t instanceCount);

    nvrhi::IDevice *_device = nullptr;
    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::BindingLayoutHandle _instanceLayout;
    nvrhi::BindingLayoutHandle _textureLayout;
    /// One per target format drawn into: the window's, and a picture's.
    std::vector<Pipeline> _pipelines;
    nvrhi::SamplerHandle _sampler;

    nvrhi::BufferHandle _instanceBuffer;
    nvrhi::BindingSetHandle _instanceSet;

    /// One binding set per registered texture, indexed by TextureId::value.
    std::vector<nvrhi::BindingSetHandle> _textureSets;

    /// What kWhiteTexture draws with.
    Render::Texture _white;

    uint32_t _instanceCapacity = 0;
    /// Where the next Draw writes its quads, counted in quads.
    uint32_t _instanceCursor = 0;
};

/// @brief Uploads @p font's atlas as a single-channel texture for Glyph quads to
/// sample. False, logged, for a font with no atlas.
[[nodiscard]] bool UploadFontAtlas(Render::Texture &texture, nvrhi::IDevice *device, const Font &font);

/// @brief Uploads the checkerboard the placeholder strip's textured tile shows.
/// Display values in a non-sRGB format, as RegisterTexture requires.
void UploadPlaceholderTexture(Render::Texture &texture, nvrhi::IDevice *device);

/// @brief Uploads the image at @p vpath for the UI, through the installed
/// asset source, as the display values its file holds in a non-sRGB format, as
/// RegisterTexture requires. Its size in pixels, or nullopt, logged, when it
/// cannot be loaded.
[[nodiscard]] std::optional<Extent> UploadUiTexture(Render::Texture &texture, nvrhi::IDevice *device,
                                                    std::string_view vpath);

/// @brief The same for the image @p id names, such as a field a texture is
/// bound to holds.
[[nodiscard]] std::optional<Extent> UploadUiTexture(Render::Texture &texture, nvrhi::IDevice *device,
                                                    Core::AssetId id);

} // namespace Assisi::Mondrian::Engine
