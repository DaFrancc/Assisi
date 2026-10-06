/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CollisionData.hpp
/// @brief The collision a model carries beside its visual geometry: hulls and
///        primitives authored as prefixed nodes, split and fitted at import.
///
/// Pure data and pure math, with no glTF and no physics engine in it, so the
/// importer, the cooked-mesh format and the physics world all read the same
/// types, and the splitting and fitting are testable without a file.
///
/// A node whose name starts with one of the prefixes below is collision, not
/// something to draw. Its triangles are welded by position and split into the
/// parts they form, so two separate blocks modelled in one node become two
/// pieces. A `UCX_` part becomes a convex hull of its points; any other prefix
/// fits that primitive to the part.

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{

struct MeshData;

/// @brief What one collision piece is.
enum class CollisionPieceKind : std::uint8_t
{
    Hull,     ///< `UCX_`: the convex hull of the part's points.
    Box,      ///< `UBX_`: a box fitted to the part's bounds.
    Sphere,   ///< `USP_`: a sphere fitted around the part.
    Capsule,  ///< `UCP_`: a capsule along the part's longest axis.
    Cylinder, ///< `UCY_`: a cylinder along the part's longest axis.
    Count,
};

/// @brief One piece of a model's collision, in the model's own space.
///
/// The piece sits at @ref position turned by @ref rotation. A hull's points and
/// a primitive's dimensions are already scaled by the node they came from, so
/// the pose is all that places them. Capsules and cylinders run along their
/// local Y axis, as the physics primitives do.
struct CollisionPiece
{
    /// The node the piece came from, with `_1`, `_2`, … when the node held
    /// several parts.
    std::string name;

    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 position{0.f};

    /// For a box.
    glm::vec3 halfExtents{0.f};

    /// For a sphere, a capsule and a cylinder.
    float radius = 0.f;

    /// For a capsule and a cylinder: half the length of the straight part.
    float halfHeight = 0.f;

    /// A hull's points, as a range of CollisionData::points, in the piece's own
    /// frame. Empty for a primitive.
    std::uint32_t firstPoint = 0;
    std::uint32_t pointCount = 0;

    CollisionPieceKind kind = CollisionPieceKind::Hull;
};

/// @brief Every collision piece a model carries. Empty when it authored none.
struct CollisionData
{
    std::vector<glm::vec3> points;
    std::vector<CollisionPiece> pieces;
};

/// @brief One prefixed node, as the importer found it: its triangles in the
///        node's own space and the matrix that places the node in the model.
struct CollisionNode
{
    std::string name;
    glm::mat4 world{1.f};
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;
    CollisionPieceKind kind = CollisionPieceKind::Hull;
};

/// @brief Why a node's collision could not be built.
enum class CollisionBuildError : std::uint8_t
{
    /// A `UCX_` part is flat, a line or a point, so no hull encloses any space.
    NoVolume,

    /// A position or the node's matrix is not a number.
    NotFinite,

    /// An index names a position the node does not have.
    IndexOutOfRange,
    Count,
};

/// @brief A short human-readable description, for the line a failed import logs.
[[nodiscard]] std::string_view ToString(CollisionBuildError error) noexcept;

/// @brief A failure, and the piece it is about, as the log line names it.
struct CollisionBuildFailure
{
    std::string piece;
    CollisionBuildError error = CollisionBuildError::NoVolume;
};

/// @brief What a node name's collision prefix asks for, or nullopt when the
///        name has none. Matched without regard to case: `ucx_` is `UCX_`.
[[nodiscard]] std::optional<CollisionPieceKind> CollisionPrefixKind(std::string_view name) noexcept;

/// @brief Welds @p node's triangles, splits them into parts and appends one
///        piece per part to @p out, in the order of each part's first triangle.
///
/// On failure @p out is left as it was.
[[nodiscard]] std::expected<void, CollisionBuildFailure> AppendCollisionPieces(const CollisionNode &node,
                                                                               CollisionData &out);

/// @brief Positions merged by place, and what each original position became.
struct WeldedPositions
{
    std::vector<glm::vec3> points;

    /// Parallel to the input: the index in @ref points each position merged into.
    std::vector<std::uint32_t> remap;
};

/// @brief Merges positions that lie in the same cell of a grid of
///        kCollisionWeldCell metres.
///
/// Exporters write a vertex once per face that uses it, wherever its normal or
/// UV differs, so the copies of one corner are separate vertices with equal
/// positions. Merging them is what makes the triangles of one block share
/// corners again.
[[nodiscard]] WeldedPositions WeldPositions(std::span<const glm::vec3> positions);

/// @brief The welding grid's cell size, in metres. Large enough to absorb the
///        rounding an exporter's transforms leave in a duplicated corner, small
///        enough that no modelled gap closes.
inline constexpr float kCollisionWeldCell = 1e-4f;

/// @brief Above this many pieces, a model logs a warning: every piece is tested
///        against everything near the body, so a model of hundreds costs far more
///        than its look suggests.
inline constexpr std::size_t kManyCollisionPieces = 32;

/// @brief A model's geometry as physics needs it: its first level of detail as
///        welded triangles, for a whole-model hull or triangle mesh, and the
///        pieces it authored.
struct CollisionModel
{
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;
    CollisionData collision;
};

/// @brief The collision model of @p mesh.
///
/// Expects explicit submesh and LOD tables, as an import or a cooked mesh has.
[[nodiscard]] CollisionModel CollisionModelOf(const MeshData &mesh);

} // namespace Assisi::Geometry
