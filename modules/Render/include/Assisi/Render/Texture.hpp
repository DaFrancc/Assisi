/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Texture.hpp
/// @brief GPU-side 2D texture storage backed by an NVRHI texture.

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Image/Image.hpp>

namespace Assisi::Render
{
/// @brief The nvrhi format an image of this pixel format and colour space
///        occupies on the GPU.
///
/// The colour space picks between an sRGB enumerator and its plain sibling, so
/// the hardware decodes to linear at sample time rather than a shader doing it.
/// Only the colour formats have an sRGB form: a single- or two-channel block
/// format carries data, and there is no gamma to undo.
[[nodiscard]] nvrhi::Format NvrhiFormatFor(Image::PixelFormat format, Image::ColorSpace colorSpace);

/// @brief Owner of an NVRHI 2D texture with a full mip chain, uncompressed or
///        block compressed.
class Texture
{
public:
    Texture() = default;

    /// @brief Create the GPU texture and upload a decoded mip chain — the
    /// main-thread half (device work). Pairs with Image::DecodeImage.
    ///
    /// When @p sharedList is non-null the upload is *recorded* into that already-open
    /// command list and NOT executed — the caller batches many uploads into one
    /// command list and submits it once (see AssetCache's shared upload list), which
    /// avoids a fresh command list + staging allocation + queue submit per texture.
    /// When null (the synchronous convenience path) a private command list is
    /// created, recorded, closed, and executed here.
    ///
    /// A chain whose buffers do not match what its format implies is refused: it
    /// is counted, warned about, and no texture is created. See UploadFailureCount.
    void UploadDecoded(nvrhi::IDevice *device, const Image::DecodedImage &image, const char *debugName = nullptr,
                       nvrhi::ICommandList *sharedList = nullptr);

    /// @brief Create the GPU texture object for a decoded image — no upload recorded.
    /// Free-threaded (nvrhi createTexture is: vkCreateImage + a per-resource memory
    /// allocation, both internally synchronized), so a decode worker can create the
    /// target while the main thread only adopts it later (see streaming P1). Pair
    /// with RecordMips into a command list, then Adopt on the main thread.
    ///
    /// Returns null for a chain that fails Image::ValidateMipChain, counting the
    /// failure as UploadDecoded does.
    static nvrhi::TextureHandle CreateImage(nvrhi::IDevice *device, const Image::DecodedImage &image,
                                            const char *debugName = nullptr);

    /// @brief Record @p image's mip uploads into @p commandList, targeting @p texture
    /// (from CreateImage). This is where the staging memcpy happens, so recording it
    /// on a worker moves that cost off the main thread. The command list must be open;
    /// the caller closes and executes it (submission stays on the main thread).
    /// Free-threaded per-command-list (each worker records into its own list).
    ///
    /// The row pitch comes from Image::LayoutFor, never from the width: a
    /// block-compressed level is addressed in block rows. A wrong one is not
    /// reported by anything and barely looks wrong either — see LayoutFor.
    static void RecordMips(nvrhi::ICommandList *commandList, nvrhi::ITexture *texture,
                           const Image::DecodedImage &image);

    /// @brief Adopt an already-created GPU texture (from CreateImage) as this
    /// texture's backing image — the main-thread publish half of the worker-recorded
    /// upload path. The pixels are uploaded when the worker's command list executes.
    void Adopt(nvrhi::TextureHandle texture) { _texture = std::move(texture); }

    /// @brief Uploads a solid 1x1 color — used for default/placeholder textures.
    /// Always uncompressed: one texel has no block to fill, and these stand in for
    /// a texture that is missing rather than carrying content of their own.
    /// @p sharedList batches the upload like UploadDecoded (null = self-contained).
    void UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                          unsigned char a, Image::ColorSpace colorSpace = Image::ColorSpace::Srgb,
                          const char *debugName = nullptr, nvrhi::ICommandList *sharedList = nullptr);

    /// @brief Uploads the magenta-and-black checkerboard that marks a texture which
    /// could not be uploaded.
    ///
    /// Deliberately loud. A refused upload that left the texture blank, or fell back
    /// to white, is invisible in a screenshot and reaches a build nobody questions;
    /// this pattern occurs in no real content and reads as broken from across a room.
    void UploadErrorPattern(nvrhi::IDevice *device, const char *debugName = nullptr,
                            nvrhi::ICommandList *sharedList = nullptr);

    /// @brief How many uploads have been refused for a malformed mip chain since
    ///        the process started.
    ///
    /// Counted rather than only logged: one warning per texture scrolls past in a
    /// load that produces hundreds of lines, and the number is what makes a
    /// regression obvious when it is reported once at the end.
    [[nodiscard]] static std::uint32_t UploadFailureCount();

    /// @brief Forget the failures counted so far, so a later load reports its own.
    static void ResetUploadFailureCount();

    nvrhi::ITexture *NativeTexture() const { return _texture; }
    bool IsValid() const { return _texture != nullptr; }

    /// @brief Sentinel for "not yet registered in a bindless descriptor table".
    static constexpr uint32_t kInvalidBindlessIndex = UINT32_MAX;

    /// @brief This texture's slot in the AssetCache's bindless descriptor table
    /// (GPU-driven stage D). Assigned once by the cache at resolve time; shared
    /// textures keep one slot. kInvalidBindlessIndex until registered.
    uint32_t BindlessIndex() const { return _bindlessIndex; }
    void SetBindlessIndex(uint32_t index) { _bindlessIndex = index; }

private:
    nvrhi::TextureHandle _texture;
    uint32_t _bindlessIndex = kInvalidBindlessIndex;
};
} /* namespace Assisi::Render */
