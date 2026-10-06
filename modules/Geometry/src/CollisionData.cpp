/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Geometry/CollisionData.hpp>

#include <Assisi/Geometry/MeshData.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace Assisi::Geometry
{

namespace
{

/// How far a point must stand off the line or plane through the others before
/// a part counts as having volume, in metres. The same order as the welding
/// grid: a part thinner than that is a modelling slip, not a wall.
constexpr float kMinPartThickness = 1e-4f;

/// A prefix and the piece it asks for.
struct CollisionPrefix
{
    std::string_view text;
    CollisionPieceKind kind;
};

constexpr std::array<CollisionPrefix, static_cast<std::size_t>(CollisionPieceKind::Count)> kPrefixes{{
    {"ucx_", CollisionPieceKind::Hull},
    {"ubx_", CollisionPieceKind::Box},
    {"usp_", CollisionPieceKind::Sphere},
    {"ucp_", CollisionPieceKind::Capsule},
    {"ucy_", CollisionPieceKind::Cylinder},
}};

bool StartsWithIgnoringCase(std::string_view text, std::string_view lowerPrefix)
{
    if (text.size() < lowerPrefix.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lowerPrefix.size(); ++i)
    {
        if (static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))) != lowerPrefix[i])
        {
            return false;
        }
    }
    return true;
}

/// One welding cell, as integer coordinates on the grid.
struct Cell
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    friend bool operator==(const Cell &, const Cell &) = default;
};

struct CellHash
{
    std::size_t operator()(const Cell &cell) const noexcept
    {
        // Three large odd multipliers spread neighbouring cells across the table.
        constexpr std::uint64_t kMixX = 0x9E3779B97F4A7C15ull;
        constexpr std::uint64_t kMixY = 0xC2B2AE3D27D4EB4Full;
        constexpr std::uint64_t kMixZ = 0x165667B19E3779F9ull;
        const std::uint64_t mixed = static_cast<std::uint64_t>(cell.x) * kMixX ^
                                    static_cast<std::uint64_t>(cell.y) * kMixY ^
                                    static_cast<std::uint64_t>(cell.z) * kMixZ;
        return static_cast<std::size_t>(mixed);
    }
};

Cell CellOf(const glm::vec3 &position)
{
    return Cell{static_cast<std::int64_t>(std::llround(position.x / kCollisionWeldCell)),
                static_cast<std::int64_t>(std::llround(position.y / kCollisionWeldCell)),
                static_cast<std::int64_t>(std::llround(position.z / kCollisionWeldCell))};
}

/// Disjoint sets over welded points, joined by the triangles that share them.
class PointSets
{
public:
    explicit PointSets(std::size_t count) : _parent(count) { std::iota(_parent.begin(), _parent.end(), 0u); }

    std::uint32_t Find(std::uint32_t point)
    {
        while (_parent[point] != point)
        {
            _parent[point] = _parent[_parent[point]];
            point = _parent[point];
        }
        return point;
    }

    void Join(std::uint32_t a, std::uint32_t b)
    {
        const std::uint32_t rootA = Find(a);
        const std::uint32_t rootB = Find(b);
        if (rootA != rootB)
        {
            _parent[std::max(rootA, rootB)] = std::min(rootA, rootB);
        }
    }

private:
    std::vector<std::uint32_t> _parent;
};

/// The welded points of one part, in the order the part's triangles first use
/// them.
using Part = std::vector<std::uint32_t>;

/// Splits the triangles over welded @p remap into the parts they form, ordered
/// by each part's first triangle.
std::vector<Part> SplitParts(std::span<const std::uint32_t> indices, const WeldedPositions &welded)
{
    PointSets sets(welded.points.size());
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        sets.Join(welded.remap[indices[i]], welded.remap[indices[i + 1]]);
        sets.Join(welded.remap[indices[i]], welded.remap[indices[i + 2]]);
    }

    using PartOfRoot = std::unordered_map<std::uint32_t, std::size_t>;
    std::vector<Part> parts;
    PartOfRoot partOfRoot;
    std::vector<bool> taken(welded.points.size(), false);
    for (const std::uint32_t index : indices)
    {
        const std::uint32_t point = welded.remap[index];
        if (taken[point])
        {
            continue;
        }
        taken[point] = true;
        const std::pair<PartOfRoot::iterator, bool> entry = partOfRoot.try_emplace(sets.Find(point), parts.size());
        if (entry.second)
        {
            parts.emplace_back();
        }
        parts[entry.first->second].push_back(point);
    }
    return parts;
}

/// Whether @p points enclose any space: four of them stand off each other's
/// point, line and plane by at least kMinPartThickness.
bool HasVolume(std::span<const glm::vec3> points)
{
    if (points.size() < 4)
    {
        return false;
    }
    const glm::vec3 origin = points.front();

    glm::vec3 far = origin;
    for (const glm::vec3 &point : points)
    {
        if (glm::distance(point, origin) > glm::distance(far, origin))
        {
            far = point;
        }
    }
    const glm::vec3 axis = far - origin;
    if (glm::length(axis) < kMinPartThickness)
    {
        return false;
    }

    glm::vec3 normal{0.f};
    for (const glm::vec3 &point : points)
    {
        const glm::vec3 candidate = glm::cross(axis, point - origin);
        if (glm::length(candidate) > glm::length(normal))
        {
            normal = candidate;
        }
    }
    // |axis × offset| is the offset's distance from the line times |axis|.
    if (glm::length(normal) < kMinPartThickness * glm::length(axis))
    {
        return false;
    }

    const glm::vec3 unitNormal = glm::normalize(normal);
    for (const glm::vec3 &point : points)
    {
        if (std::abs(glm::dot(point - origin, unitNormal)) >= kMinPartThickness)
        {
            return true;
        }
    }
    return false;
}

/// A node's matrix as a pose and a scale. Shear is not kept: a node's pieces
/// are rigid shapes, and nothing places a sheared one.
struct NodeFrame
{
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 position{0.f};
    glm::vec3 scale{1.f};
};

NodeFrame FrameOf(const glm::mat4 &world)
{
    const glm::mat3 linear(world);
    glm::vec3 scale(glm::length(linear[0]), glm::length(linear[1]), glm::length(linear[2]));
    // A mirroring matrix keeps its mirror in the scale, so the rotation stays a
    // rotation.
    if (glm::determinant(linear) < 0.f)
    {
        scale.x = -scale.x;
    }
    glm::mat3 rotation(1.f);
    for (glm::length_t column = 0; column < 3; ++column)
    {
        if (scale[column] != 0.f)
        {
            rotation[column] = linear[column] / scale[column];
        }
    }
    return NodeFrame{glm::normalize(glm::quat_cast(rotation)), glm::vec3(world[3]), scale};
}

bool IsFinite(const glm::vec3 &value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(const glm::mat4 &matrix)
{
    for (glm::length_t column = 0; column < 4; ++column)
    {
        if (!IsFinite(glm::vec3(matrix[column])) || !std::isfinite(matrix[column].w))
        {
            return false;
        }
    }
    return true;
}

/// The bounds of @p part's points, each scaled into the node's frame.
Aabb ScaledBoundsOf(const Part &part, std::span<const glm::vec3> points)
{
    Aabb box{.min = glm::vec3(std::numeric_limits<float>::max()),
             .max = glm::vec3(std::numeric_limits<float>::lowest())};
    for (const std::uint32_t point : part)
    {
        box.min = glm::min(box.min, points[point]);
        box.max = glm::max(box.max, points[point]);
    }
    return box;
}

/// A rotation carrying local Y onto the axis of @p extent that is longest.
glm::quat AlongLongestAxis(const glm::vec3 &extent, glm::length_t &longest)
{
    longest = 1;
    if (extent.x > extent[longest])
    {
        longest = 0;
    }
    if (extent.z > extent[longest])
    {
        longest = 2;
    }
    switch (longest)
    {
    case 0:
        return glm::angleAxis(-glm::half_pi<float>(), glm::vec3(0.f, 0.f, 1.f));
    case 2:
        return glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.f, 0.f, 0.f));
    default:
        return glm::quat(1.f, 0.f, 0.f, 0.f);
    }
}

/// Fits @p piece's primitive to @p part, whose points are scaled into the
/// node's frame, and places it in the model.
void FitPrimitive(const Part &part, std::span<const glm::vec3> points, const NodeFrame &frame,
                  CollisionPiece &piece)
{
    const Aabb box = ScaledBoundsOf(part, points);
    const glm::vec3 centre = (box.min + box.max) * 0.5f;
    const glm::vec3 half = (box.max - box.min) * 0.5f;
    piece.position = frame.position + frame.rotation * centre;
    piece.rotation = frame.rotation;

    switch (piece.kind)
    {
    case CollisionPieceKind::Box:
        piece.halfExtents = half;
        return;
    case CollisionPieceKind::Sphere:
    {
        float radiusSquared = 0.f;
        for (const std::uint32_t point : part)
        {
            const glm::vec3 offset = points[point] - centre;
            radiusSquared = std::max(radiusSquared, glm::dot(offset, offset));
        }
        piece.radius = std::sqrt(radiusSquared);
        return;
    }
    case CollisionPieceKind::Capsule:
    case CollisionPieceKind::Cylinder:
    {
        glm::length_t longest = 1;
        piece.rotation = frame.rotation * AlongLongestAxis(half, longest);
        const float across = std::max(half[(longest + 1) % 3], half[(longest + 2) % 3]);
        piece.radius = across;
        piece.halfHeight = piece.kind == CollisionPieceKind::Cylinder ? half[longest]
                                                                      : std::max(half[longest] - across, 0.f);
        return;
    }
    case CollisionPieceKind::Hull:
    case CollisionPieceKind::Count:
        return;
    }
}

std::string PartName(const CollisionNode &node, std::size_t part, std::size_t parts)
{
    return parts == 1 ? node.name : std::format("{}_{}", node.name, part + 1);
}

} // namespace

std::string_view ToString(CollisionBuildError error) noexcept
{
    switch (error)
    {
    case CollisionBuildError::NoVolume:
        return "a convex part is flat, so no hull encloses it";
    case CollisionBuildError::NotFinite:
        return "a position or the node's transform is not a number";
    case CollisionBuildError::IndexOutOfRange:
        return "an index names a position the node does not have";
    case CollisionBuildError::Count:
        break;
    }
    return "unknown";
}

std::optional<CollisionPieceKind> CollisionPrefixKind(std::string_view name) noexcept
{
    for (const CollisionPrefix &prefix : kPrefixes)
    {
        if (StartsWithIgnoringCase(name, prefix.text))
        {
            return prefix.kind;
        }
    }
    return std::nullopt;
}

WeldedPositions WeldPositions(std::span<const glm::vec3> positions)
{
    using PointOfCell = std::unordered_map<Cell, std::uint32_t, CellHash>;
    WeldedPositions welded;
    welded.remap.reserve(positions.size());
    PointOfCell pointOfCell;
    for (const glm::vec3 &position : positions)
    {
        const std::pair<PointOfCell::iterator, bool> entry =
            pointOfCell.try_emplace(CellOf(position), static_cast<std::uint32_t>(welded.points.size()));
        if (entry.second)
        {
            welded.points.push_back(position);
        }
        welded.remap.push_back(entry.first->second);
    }
    return welded;
}

std::expected<void, CollisionBuildFailure> AppendCollisionPieces(const CollisionNode &node, CollisionData &out)
{
    if (!IsFinite(node.world))
    {
        return std::unexpected(CollisionBuildFailure{node.name, CollisionBuildError::NotFinite});
    }
    for (const glm::vec3 &position : node.positions)
    {
        if (!IsFinite(position))
        {
            return std::unexpected(CollisionBuildFailure{node.name, CollisionBuildError::NotFinite});
        }
    }
    for (const std::uint32_t index : node.indices)
    {
        if (index >= node.positions.size())
        {
            return std::unexpected(CollisionBuildFailure{node.name, CollisionBuildError::IndexOutOfRange});
        }
    }

    const NodeFrame frame = FrameOf(node.world);
    std::vector<glm::vec3> scaled(node.positions.size());
    for (std::size_t i = 0; i < node.positions.size(); ++i)
    {
        scaled[i] = node.positions[i] * frame.scale;
    }
    const WeldedPositions welded = WeldPositions(scaled);
    const std::vector<Part> parts = SplitParts(node.indices, welded);

    CollisionData appended;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        CollisionPiece piece;
        piece.name = PartName(node, i, parts.size());
        piece.kind = node.kind;
        if (node.kind != CollisionPieceKind::Hull)
        {
            FitPrimitive(parts[i], welded.points, frame, piece);
            appended.pieces.push_back(std::move(piece));
            continue;
        }

        std::vector<glm::vec3> hull;
        hull.reserve(parts[i].size());
        for (const std::uint32_t point : parts[i])
        {
            hull.push_back(welded.points[point]);
        }
        if (!HasVolume(hull))
        {
            return std::unexpected(CollisionBuildFailure{piece.name, CollisionBuildError::NoVolume});
        }
        piece.position = frame.position;
        piece.rotation = frame.rotation;
        piece.firstPoint = static_cast<std::uint32_t>(out.points.size() + appended.points.size());
        piece.pointCount = static_cast<std::uint32_t>(hull.size());
        appended.points.insert(appended.points.end(), hull.begin(), hull.end());
        appended.pieces.push_back(std::move(piece));
    }

    out.points.insert(out.points.end(), appended.points.begin(), appended.points.end());
    out.pieces.insert(out.pieces.end(), std::make_move_iterator(appended.pieces.begin()),
                      std::make_move_iterator(appended.pieces.end()));
    return {};
}

CollisionModel CollisionModelOf(const MeshData &mesh)
{
    CollisionModel model;
    model.collision = mesh.Collision;
    if (mesh.Lods.empty())
    {
        return model;
    }

    std::vector<glm::vec3> positions;
    positions.reserve(mesh.Vertices.size());
    for (const Vertex &vertex : mesh.Vertices)
    {
        positions.push_back(vertex.Position);
    }
    const WeldedPositions welded = WeldPositions(positions);

    // Only the points the first level uses: a coarser level's vertices would
    // swell the hull and the triangle mesh with geometry nothing draws up close.
    std::vector<std::uint32_t> compact(welded.points.size(), std::numeric_limits<std::uint32_t>::max());
    const LodRange &lod = mesh.Lods.front();
    for (std::uint32_t s = lod.FirstSubMesh; s < lod.FirstSubMesh + lod.SubMeshCount; ++s)
    {
        const SubMesh &submesh = mesh.SubMeshes[s];
        for (std::uint32_t i = submesh.IndexOffset; i < submesh.IndexOffset + submesh.IndexCount; ++i)
        {
            const std::uint32_t point = welded.remap[mesh.Indices[i]];
            if (compact[point] == std::numeric_limits<std::uint32_t>::max())
            {
                compact[point] = static_cast<std::uint32_t>(model.positions.size());
                model.positions.push_back(welded.points[point]);
            }
            model.indices.push_back(compact[point]);
        }
    }
    return model;
}

} // namespace Assisi::Geometry
