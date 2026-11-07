/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FogVolume.hpp
/// @brief Fog a designer places: a box, an ellipsoid, or an outline filled to a
/// height, each with soft edges, a colour and a thickness of its own.
///
/// **These functions are the specification and sky/fog_volumes.glsl is their
/// transcription**, as Fog.hpp is for the level's fog. The tests hold the CPU
/// form; the shader has to agree with it.
///
/// Not volumetric. Each pixel works out how much fog its line of sight passes
/// through in front of the surface it shows, from the shape alone, and fogs it
/// by that much. Nothing casts a shadow into it.
///
/// Every volume lives in a frame of its own, turned and moved but never scaled,
/// so a length there is metres. In that frame it is a box of some half extents,
/// and its kind says which part of the box holds fog: all of it, the ellipsoid
/// touching its faces, or an outline in its x-z plane from its floor to its top.
///
/// Edges are soft: within the edge softness of a face, a wall, a floor or a top,
/// the fog thins linearly to nothing at the boundary. Where two boundaries'
/// softness overlaps, along an edge or at a corner, the two thinnings multiply,
/// so the fog there is thinner than near either and never less than none. A
/// falloff thins the fog upward from the floor besides, as the level's mist
/// thins above its height.
///
/// Along a line of sight the fog is a polynomial between the points where its
/// formula changes, at the inner edges of the softness, times the falloff. Each
/// piece is integrated by the two-point Gauss-Legendre rule, which is exact for
/// the cubics a volume without falloff makes, in steps short enough that the
/// falloff costs it no more than half a percent.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/GpuLayout.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Assisi::Render
{

/// @brief The side of a square of pixels that lists the volumes covering it. A
/// pixel tests only its own square's volumes, so a volume costs in proportion to
/// how much of the screen it covers.
inline constexpr uint32_t kFogTilePixels = 64u;

/// @brief The most squares the screen is cut into. A screen that would need more
/// gets larger squares instead.
inline constexpr uint32_t kMaxFogTiles = 8192u;

/// @brief The most volumes drawn in one frame. Past it, volumes are dropped.
inline constexpr uint32_t kMaxFogVolumes = 256u;

/// @brief The most outline edges drawn in one frame, over every outline volume.
inline constexpr uint32_t kMaxFogOutlineEdges = 8192u;

/// @brief The most volume entries every square's list holds between them. Past
/// it, the squares listed last lose volumes.
inline constexpr uint32_t kMaxFogTileEntries = 65536u;

/// @brief A falloff of zero keeps a volume's fog as thick at its top as at its
/// floor; any other is clamped to this range, in metres.
inline constexpr float kMinFogVolumeFalloff = 0.05f;
inline constexpr float kMaxFogVolumeFalloff = 100000.0f;

/// @brief A falloff of zero, or one inside the range.
[[nodiscard]] float SanitizedFogVolumeFalloff(float falloff);

/// @brief Which part of its box a volume fills. Must match sky/fog_volumes.glsl.
enum class FogVolumeKind : uint32_t
{
    Box,
    Ellipsoid,
    Outline,
    Count_,
};

/// @brief One volume as the shader reads it. Mirrors sky/fog_volumes.glsl's
/// FogVolume; the member order is the layout.
struct FogVolumeGpu
{
    /// The rows of the transform from the lit pass's space into the volume's
    /// frame: a point's frame coordinate i is dot(xyz, point) + w.
    std::array<glm::vec4, 3> toFrame{};
    /// xyz = the box's half extents in metres, w = the edge softness in metres.
    glm::vec4 extents{0.0f};
    /// xyz = linear colour, w = optical depth per metre where the fog is full.
    glm::vec4 color{0.0f};
    /// x = metres above the floor for the fog to thin to about a third (1/e),
    /// zero for even; yzw unused.
    glm::vec4 profile{0.0f};
    /// x = FogVolumeKind, y = 1 while lit by the sky, z = the first outline edge,
    /// w = how many outline edges.
    glm::uvec4 info{0u};
};

ASSISI_GPU_LAYOUT(FogVolumeGpu);
ASSISI_GPU_FIRST_FIELD(FogVolumeGpu, toFrame);
ASSISI_GPU_FIELD_AFTER(FogVolumeGpu, extents, toFrame);
ASSISI_GPU_FIELD_AFTER(FogVolumeGpu, color, extents);
ASSISI_GPU_FIELD_AFTER(FogVolumeGpu, profile, color);
ASSISI_GPU_FIELD_AFTER(FogVolumeGpu, info, profile);
ASSISI_GPU_NO_TAIL_PADDING(FogVolumeGpu, info);

/// @brief One edge of an outline, in its volume's frame, with x-z as a plane's
/// x-y. Mirrors sky/fog_volumes.glsl's FogOutlineEdge.
///
/// Outlines run anticlockwise seen from above, so the inside is on each edge's
/// left. The softness band along an edge ends at the lines bisecting the corners
/// at its two ends, so neighbouring edges' bands meet without a gap or an overlap.
struct FogOutlineEdgeGpu
{
    /// xy = where the edge starts, zw = where it ends.
    glm::vec4 points{0.0f};
    /// xy = the unit normal pointing inside; zw unused.
    glm::vec4 normal{0.0f};
    /// xy = the normal of the line bisecting the corner at the start, pointing
    /// along the edge; zw = the same at the end.
    glm::vec4 miters{0.0f};
};

ASSISI_GPU_LAYOUT(FogOutlineEdgeGpu);
ASSISI_GPU_FIRST_FIELD(FogOutlineEdgeGpu, points);
ASSISI_GPU_FIELD_AFTER(FogOutlineEdgeGpu, normal, points);
ASSISI_GPU_FIELD_AFTER(FogOutlineEdgeGpu, miters, normal);
ASSISI_GPU_NO_TAIL_PADDING(FogOutlineEdgeGpu, miters);

/// @brief A volume as its owner describes it.
struct FogVolumeDesc
{
    /// The volume's frame in the lit pass's space: a rotation and a translation,
    /// no scale. The box is centred on its origin.
    glm::mat4 frame{1.0f};
    glm::vec3 halfExtents{0.0f};
    /// Linear. With @ref litBySky, a tint over the sky's light.
    glm::vec3 color{1.0f};
    /// How far, in metres, the fog lets the eye see where it is full: 99% of
    /// what lies that far into it is hidden. Zero or less is no fog.
    float visibility = 0.0f;
    /// How deep, in metres, the fog thins toward a boundary.
    float edgeSoftness = 0.0f;
    /// How many metres above the box's floor the fog takes to thin to about a
    /// third (1/e). Zero keeps it even.
    float falloff = 0.0f;
    /// For an outline: its edges, as AppendFogOutline placed them.
    uint32_t firstEdge = 0u;
    uint32_t edgeCount = 0u;
    FogVolumeKind kind = FogVolumeKind::Box;
    /// Whether @ref color tints the light the level's haze takes from the sky,
    /// rather than being drawn as it is.
    bool litBySky = true;
};

/// @brief The volume @p desc describes, as the shader reads it.
[[nodiscard]] FogVolumeGpu MakeFogVolume(const FogVolumeDesc &desc);

/// @brief Where AppendFogOutline put an outline, and the box around it.
struct FogOutlinePlacement
{
    /// The middle of the outline's bounds, in the coordinates it was given in.
    /// The edges are stored around it, so it is where the volume's frame goes.
    glm::vec2 centre{0.0f};
    glm::vec2 halfExtents{0.0f};
    uint32_t firstEdge = 0u;
    uint32_t edgeCount = 0u;
};

/// @brief Adds the closed outline through @p points to @p edges, anticlockwise
/// and around the middle of its bounds.
///
/// Repeated points and a closing point equal to the first are dropped. Nothing
/// is added for fewer than three points, for an outline with no area, or for one
/// that would take @p edges past kMaxFogOutlineEdges.
[[nodiscard]] std::optional<FogOutlinePlacement> AppendFogOutline(std::vector<FogOutlineEdgeGpu> &edges,
                                                                  std::span<const glm::vec2> points);

/// @brief How much of a volume a line of sight passes through.
struct FogSpan
{
    /// Metres of full fog it amounts to: the length inside, less what the soft
    /// edges thin away.
    float length = 0.0f;
    /// How far along the line of sight it first reaches the volume's box.
    float entry = 0.0f;
};

/// @brief What @p volume does to the line of sight from @p eye along the unit
/// @p ray, as far as @p distance (any amount, infinite included, for the sky).
/// @p edges is the frame's outline edges.
[[nodiscard]] FogSpan FogVolumeSpan(const FogVolumeGpu &volume, std::span<const FogOutlineEdgeGpu> edges,
                                    const glm::vec3 &eye, const glm::vec3 &ray, float distance);

/// @brief The colour @p volume's fog shows: its own, or a tint over @p haze,
/// the level haze's colour along the ray; then seen through @p hazeOpacity of
/// that haze in front of it.
[[nodiscard]] glm::vec3 FogVolumeColor(const FogVolumeGpu &volume, const glm::vec3 &haze, float hazeOpacity);

/// @brief The volumes a pixel's line of sight crosses, gathered in any order.
///
/// Each adds its optical depth and its colour weighted by it. Over() treats the
/// lot as one fog of the summed depth and the averaged colour, which is exact
/// where volumes overlap and leaves which one is in front out of it, so no pixel
/// has to sort them.
struct FogVolumeBlend
{
    glm::vec3 weighted{0.0f};
    float depth = 0.0f;

    void Add(float opticalDepth, const glm::vec3 &color);

    /// @brief @p behind seen through every volume added.
    [[nodiscard]] glm::vec3 Over(const glm::vec3 &behind) const;
};

/// @brief How the screen is cut into squares for the volume lists.
struct FogTileGrid
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t tilePixels = kFogTilePixels;
    uint32_t columns = 0u;
    uint32_t rows = 0u;
};

/// @brief The squares a @p width by @p height screen is cut into:
/// kFogTilePixels on a side, or larger where that would make more than
/// kMaxFogTiles.
[[nodiscard]] FogTileGrid FogTileGridFor(uint32_t width, uint32_t height);

/// @brief A block of squares, both corners included.
struct FogTileRect
{
    uint32_t minColumn = 0u;
    uint32_t minRow = 0u;
    uint32_t maxColumn = 0u;
    uint32_t maxRow = 0u;
};

/// @brief The squares a volume's box may cover on screen, or none when it is
/// wholly off screen.
///
/// @p frame places the box, of @p halfExtents, in the space @p viewProjection
/// draws. A box reaching behind the eye may cover any square, so it lists in all
/// of them unless it is wholly to one side.
[[nodiscard]] std::optional<FogTileRect> FogVolumeTileRect(const glm::mat4 &viewProjection, const glm::mat4 &frame,
                                                           const glm::vec3 &halfExtents, const FogTileGrid &grid);

/// @brief Each square's list of volumes, as the shader reads it: an offset and a
/// count per square, row by row, then the volume indices the offsets point at.
/// Volume i covers @p rects[i].
void BuildFogTileLists(const FogTileGrid &grid, std::span<const FogTileRect> rects, std::vector<uint32_t> &out);

/// @brief One frame's volumes, ready to upload.
struct FogVolumeFrame
{
    std::vector<FogVolumeGpu> volumes;
    std::vector<FogOutlineEdgeGpu> edges;
    /// What BuildFogTileLists wrote.
    std::vector<uint32_t> tiles;
    FogTileGrid grid;

    void Clear();

    /// @brief What the shaders' fogVolumes lanes hold: x = how many volumes, so
    /// zero skips them all, y = columns, z = rows, w = the square's side.
    [[nodiscard]] glm::uvec4 Lanes() const;
};

} // namespace Assisi::Render
