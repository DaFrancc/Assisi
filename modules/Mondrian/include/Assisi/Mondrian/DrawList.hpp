/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file DrawList.hpp
/// @brief What the UI hands the renderer each frame: quad instances in window
/// pixels, grouped into draws that share a texture.
///
/// Plain data with no GPU types, so the core can build and test it headless and
/// the engine layer is the only thing that knows how it reaches the screen. The
/// instance struct is nonetheless laid out exactly as the shader reads it, so
/// the engine uploads it without translating.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Assisi::Mondrian
{

/// @brief A rectangle in window pixels, origin top-left, y down.
struct Rect
{
    float x      = 0.f;
    float y      = 0.f;
    float width  = 0.f;
    float height = 0.f;
};

/// @brief A display-space colour with straight (not premultiplied) alpha. The
/// shader premultiplies, so authored colours read the way they are written.
struct Color
{
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
};

/// @brief The size of the surface the UI lays out against, in pixels.
struct Extent
{
    uint32_t width  = 0;
    uint32_t height = 0;
};

/// @brief A texture the engine layer registered for the UI. The UI owns its
/// textures, so these never alias the level's.
struct TextureId
{
    uint32_t value = 0;
    bool operator==(const TextureId &) const = default;
};

/// @brief Reserved: a custom material a draw can use in place of the built-in shader.
struct MaterialId
{
    uint32_t value = 0;
    bool operator==(const MaterialId &) const = default;
};

/// @brief Reserved: a mask a draw is clipped through.
struct MaskId
{
    uint32_t value = 0;
    bool operator==(const MaskId &) const = default;
};

/// The texture every draw starts with: opaque white, so an untextured quad
/// samples the identity.
inline constexpr TextureId kWhiteTexture{};
inline constexpr MaterialId kNoMaterial{};
inline constexpr MaskId kNoMask{};

/// The transform slot holding identity, which every quad starts with.
inline constexpr uint32_t kIdentityTransform = 0;

/// How the shader fills a quad. The values are read by the shader.
enum class QuadKind : uint32_t
{
    Solid,     ///< colour only; the texture is ignored
    Glyph,     ///< colour times the coverage in the texture's red channel
    Image,     ///< colour times the texture
    NineSlice, ///< an image whose borders keep their size; drawn as Image for now
    Count
};

/// The shape of one corner. The values are read by the shader.
enum class CornerStyle : uint32_t
{
    Square,
    Rounded,
    Cut, ///< a straight chamfer across the corner
    Count
};

/// Corner order in QuadInstance::cornerRadius and the packed style word.
enum class Corner : uint32_t
{
    TopLeft,
    TopRight,
    BottomRight,
    BottomLeft,
    Count
};

/// Bits one corner's style takes in QuadInstance::cornerStyles.
inline constexpr uint32_t kCornerStyleBits = 2;
inline constexpr uint32_t kCornerStyleMask = (1u << kCornerStyleBits) - 1u;
static_assert(static_cast<uint32_t>(CornerStyle::Count) <= (1u << kCornerStyleBits));
static_assert(static_cast<uint32_t>(Corner::Count) * kCornerStyleBits <= 32u);

/// @brief @p packed with @p corner's style replaced by @p style.
[[nodiscard]] constexpr uint32_t PackCornerStyle(uint32_t packed, Corner corner, CornerStyle style)
{
    const uint32_t shift = static_cast<uint32_t>(corner) * kCornerStyleBits;
    return (packed & ~(kCornerStyleMask << shift)) | (static_cast<uint32_t>(style) << shift);
}

[[nodiscard]] constexpr CornerStyle UnpackCornerStyle(uint32_t packed, Corner corner)
{
    const uint32_t shift = static_cast<uint32_t>(corner) * kCornerStyleBits;
    return static_cast<CornerStyle>((packed >> shift) & kCornerStyleMask);
}

/// Half the side of the clip rect a quad starts with, in pixels: far larger
/// than any window, and still small enough that the shader's arithmetic on it
/// stays exact.
inline constexpr float kNoClipHalfExtent = 1.0e6f;
inline constexpr Rect kNoClip{.x      = -kNoClipHalfExtent,
                              .y      = -kNoClipHalfExtent,
                              .width  = 2.f * kNoClipHalfExtent,
                              .height = 2.f * kNoClipHalfExtent};

/// @brief One quad as the shader reads it.
///
/// The shader sees the instance buffer as an array of vec4, and reads each
/// field from the vec4 slot InstanceSlot names; the asserts below hold the
/// C++ side to the same slots. Largest-first, with no padding anywhere.
struct QuadInstance
{
    Rect rect;
    Rect uv{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};
    Rect clip = kNoClip;
    Color color{.r = 1.f, .g = 1.f, .b = 1.f, .a = 1.f};
    Color borderColor{.r = 0.f, .g = 0.f, .b = 0.f, .a = 0.f};
    std::array<float, static_cast<std::size_t>(Corner::Count)> cornerRadius{};
    float borderWidth       = 0.f;
    uint32_t cornerStyles   = 0; ///< one CornerStyle per corner, packed by PackCornerStyle
    uint32_t kind           = static_cast<uint32_t>(QuadKind::Solid);
    uint32_t transformIndex = kIdentityTransform; ///< reserved; always identity for now
};

/// The vec4 slots of a QuadInstance, mirrored as constants in mondrian/quad.glsl.
enum class InstanceSlot : uint32_t
{
    Rect,
    Uv,
    Clip,
    Color,
    BorderColor,
    CornerRadius,
    Scalars, ///< borderWidth, cornerStyles, kind, transformIndex
    Count
};

inline constexpr std::size_t kInstanceSlotBytes = 16;

[[nodiscard]] constexpr std::size_t SlotOffset(InstanceSlot slot)
{
    return static_cast<std::size_t>(slot) * kInstanceSlotBytes;
}

static_assert(sizeof(QuadInstance) == SlotOffset(InstanceSlot::Count));
static_assert(offsetof(QuadInstance, rect) == SlotOffset(InstanceSlot::Rect));
static_assert(offsetof(QuadInstance, uv) == SlotOffset(InstanceSlot::Uv));
static_assert(offsetof(QuadInstance, clip) == SlotOffset(InstanceSlot::Clip));
static_assert(offsetof(QuadInstance, color) == SlotOffset(InstanceSlot::Color));
static_assert(offsetof(QuadInstance, borderColor) == SlotOffset(InstanceSlot::BorderColor));
static_assert(offsetof(QuadInstance, cornerRadius) == SlotOffset(InstanceSlot::CornerRadius));
static_assert(offsetof(QuadInstance, borderWidth) == SlotOffset(InstanceSlot::Scalars));
static_assert(offsetof(QuadInstance, cornerStyles) == SlotOffset(InstanceSlot::Scalars) + sizeof(float));
static_assert(offsetof(QuadInstance, kind) == SlotOffset(InstanceSlot::Scalars) + 2 * sizeof(float));
static_assert(offsetof(QuadInstance, transformIndex) == SlotOffset(InstanceSlot::Scalars) + 3 * sizeof(float));

/// @brief A run of consecutive instances drawn with one texture, material and mask.
struct DrawEntry
{
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    TextureId texture      = kWhiteTexture;
    MaterialId material    = kNoMaterial;
    MaskId mask            = kNoMask;
};

class DrawList;

/// @brief Sets the properties of the quad DrawList::Quad just added. Use it
/// within the statement that made it: it addresses the quad by index, and the
/// list stops accepting changes once finalized.
class QuadBuilder
{
public:
    QuadBuilder &Fill(const Color &color);
    QuadBuilder &Border(float width, const Color &color);
    /// @brief The same radius and style on all four corners.
    QuadBuilder &Corners(float radius, CornerStyle style);
    QuadBuilder &CornerAt(Corner corner, float radius, CornerStyle style);
    /// @brief Only the part of the quad inside @p clip is drawn.
    QuadBuilder &Clip(const Rect &clip);
    /// @brief Samples @p texture over @p uv, and makes the quad an Image.
    QuadBuilder &Texture(TextureId texture, const Rect &uv);
    QuadBuilder &Kind(QuadKind kind);
    QuadBuilder &Material(MaterialId material);
    QuadBuilder &Mask(MaskId mask);

private:
    friend class DrawList;
    QuadBuilder(DrawList &list, std::size_t index) : _list(&list), _index(index) {}

    [[nodiscard]] QuadInstance &Instance();

    DrawList *_list;
    std::size_t _index;
};

/// @brief The quads to draw this frame, back to front.
///
/// Built with Quad(), then Finalize() drops what cannot be seen and groups the
/// rest into draws. The engine draws only a finalized list.
class DrawList
{
public:
    /// @brief Adds a quad covering @p rect: opaque white, square, unclipped,
    /// untextured. Must not be called once finalized.
    QuadBuilder Quad(const Rect &rect);

    /// @brief Removes every quad with no area or wholly outside its clip, then
    /// groups consecutive quads that share a texture, material and mask.
    void Finalize();

    /// @brief Empties the list and makes it accept quads again.
    void Clear();

    [[nodiscard]] bool IsFinalized() const { return _finalized; }
    [[nodiscard]] std::span<const QuadInstance> Instances() const { return _instances; }
    [[nodiscard]] std::span<const DrawEntry> Entries() const { return _entries; }

private:
    friend class QuadBuilder;

    /// What a quad is drawn with, kept beside the instance rather than in it:
    /// the shader never reads these, the grouping does.
    struct Binding
    {
        TextureId texture   = kWhiteTexture;
        MaterialId material = kNoMaterial;
        MaskId mask         = kNoMask;
    };

    std::vector<QuadInstance> _instances;
    std::vector<Binding> _bindings;
    std::vector<DrawEntry> _entries;
    bool _finalized = false;
};

} // namespace Assisi::Mondrian
