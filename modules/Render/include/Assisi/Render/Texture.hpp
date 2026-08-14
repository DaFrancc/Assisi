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

namespace Assisi::Render
{
/// @brief How a texture's stored 8-bit values map to the linear values shaders
/// work in. Colour/albedo maps are authored in sRGB (Srgb) so the GPU decodes
/// them to linear at sample time — which also makes hardware filtering and mip
/// generation correct, since both must happen in linear space. Data maps
/// (normals, metallic/roughness, masks) hold linear values already and must use
/// Linear so they aren't gamma-mangled.
enum class ColorSpace : std::uint8_t
{
    Srgb,
    Linear,
};

/// @brief A CPU-side decoded image: the full RGBA8 mip chain, ready to upload.
/// Produced by Texture::DecodeImage (which does the file read, decode, and mip
/// downsampling — all CPU, no device) so that work can run on a worker thread;
/// consumed by Texture::UploadDecoded on the main thread. `mips[0]` is the base
/// level; each entry is `width_i * height_i * 4` bytes (levels halve down to 1x1).
struct DecodedImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    ColorSpace colorSpace = ColorSpace::Srgb;
    std::vector<std::vector<unsigned char>> mips;
};

/// @brief Owner of an NVRHI 2D texture, RGBA8, with a full mip chain.
class Texture
{
public:
    Texture() = default;

    /// @brief Decode an image from a virtual asset path into a CPU mip chain — the
    /// worker-thread half of async texture loading. Does the AssetSystem resolve,
    /// stb_image decode (always to 4-channel RGBA8, top-down to match Vulkan's V=0
    /// convention), and CPU mip generation (stb's colour-space-aware resizer, so
    /// sRGB is filtered in linear space). Touches no device and no shared state, so
    /// it is safe to call from any thread. @p colorSpace selects the filtering
    /// space and is carried into the result for UploadDecoded.
    /// @return the decoded chain, or an AssetError if the file can't be resolved/read/decoded.
    static std::expected<DecodedImage, Assisi::Core::AssetError> DecodeImage(std::string_view vpath,
                                                                             ColorSpace colorSpace) noexcept;

    /// @brief Decode every frame of an animated WebP into a list of CPU images —
    /// one DecodedImage per frame, in playback order. stb_image can't read WebP, so
    /// this goes through libwebp's demux/AnimDecoder (which composites disposal and
    /// blending for us, yielding full-canvas RGBA frames). Each frame is a single
    /// mip level (no chain: the spinner is drawn at roughly its native size), tagged
    /// @p colorSpace. A still (single-frame) WebP decodes to a one-element list.
    /// Touches no device, so it is safe on a worker thread.
    /// @return the frames in order, or an AssetError if the file can't be
    ///         resolved/read/decoded.
    static std::expected<std::vector<DecodedImage>, Assisi::Core::AssetError>
    DecodeAnimatedWebp(std::string_view vpath, ColorSpace colorSpace = ColorSpace::Srgb) noexcept;

    /// @brief Create the GPU texture and upload a decoded mip chain — the
    /// main-thread half (device work). Pairs with DecodeImage.
    ///
    /// When @p sharedList is non-null the upload is *recorded* into that already-open
    /// command list and NOT executed — the caller batches many uploads into one
    /// command list and submits it once (see AssetCache's shared upload list), which
    /// avoids a fresh command list + staging allocation + queue submit per texture.
    /// When null (the synchronous convenience path) a private command list is
    /// created, recorded, closed, and executed here as before.
    void UploadDecoded(nvrhi::IDevice *device, const DecodedImage &image, const char *debugName = nullptr,
                       nvrhi::ICommandList *sharedList = nullptr);

    /// @brief Create the GPU texture object for a decoded image — no upload recorded.
    /// Free-threaded (nvrhi createTexture is: vkCreateImage + a per-resource memory
    /// allocation, both internally synchronized), so a decode worker can create the
    /// target while the main thread only adopts it later (see streaming P1). Pair
    /// with RecordMips into a command list, then Adopt on the main thread.
    static nvrhi::TextureHandle CreateImage(nvrhi::IDevice *device, const DecodedImage &image,
                                            const char *debugName = nullptr);

    /// @brief Record @p image's mip uploads into @p commandList, targeting @p texture
    /// (from CreateImage). This is where the staging memcpy happens, so recording it
    /// on a worker moves that cost off the main thread. The command list must be open;
    /// the caller closes and executes it (submission stays on the main thread).
    /// Free-threaded per-command-list (each worker records into its own list).
    static void RecordMips(nvrhi::ICommandList *commandList, nvrhi::ITexture *texture, const DecodedImage &image);

    /// @brief Adopt an already-created GPU texture (from CreateImage) as this
    /// texture's backing image — the main-thread publish half of the worker-recorded
    /// upload path. The pixels are uploaded when the worker's command list executes.
    void Adopt(nvrhi::TextureHandle texture) { _texture = std::move(texture); }

    /// @brief Loads an image from a virtual asset path (via stb_image) and uploads it.
    /// Synchronous convenience = DecodeImage + UploadDecoded on the calling thread;
    /// the async path calls the two halves separately (worker decode, main upload).
    ///
    /// Always decoded to 4 channels (RGBA8). Loaded top-down, matching Vulkan/NVRHI's
    /// UV convention (V=0 at the top). A full mip chain is generated on the CPU (with
    /// stb's colour-space-aware resizer) so distant surfaces don't alias; @p colorSpace
    /// selects the on-GPU format (SRGBA8 for albedo, RGBA8 for data maps) and the
    /// filtering space used to build the mips.
    ///
    /// @return Success, or an AssetError if the file cannot be resolved/read/decoded.
    std::expected<void, Assisi::Core::AssetError> LoadFromAssets(nvrhi::IDevice *device, std::string_view vpath,
                                                                 ColorSpace colorSpace = ColorSpace::Srgb) noexcept;

    /// @brief Uploads a solid 1x1 color — used for default/placeholder textures.
    /// Single mip level (nothing to downsample). @p sharedList batches the upload
    /// like UploadDecoded (null = self-contained, execute here).
    void UploadSolidColor(nvrhi::IDevice *device, unsigned char r, unsigned char g, unsigned char b,
                          unsigned char a, ColorSpace colorSpace = ColorSpace::Srgb,
                          const char *debugName = nullptr, nvrhi::ICommandList *sharedList = nullptr);

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
