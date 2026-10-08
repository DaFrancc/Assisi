/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshData.hpp
/// @brief CPU-side mesh representation — the decode target for mesh importers
///        and the input to Render's MeshBuffer::Upload().
///
/// This lives in Geometry (not Render) on purpose: it is pure CPU data with no
/// GPU dependency, so importers, tools, and tests can produce or read geometry
/// without linking the renderer.
///
/// One vertex array + one index array per mesh asset, addressed through
/// SubMesh index ranges. SubMesh offsets are relative, so the data
/// sub-allocates into Render's shared GeometryArena by adding a base offset
/// without any consumer changing.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Geometry/CollisionData.hpp>
#include <Assisi/Geometry/MaterialData.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Geometry
{
/// @brief A single vertex with position, surface normal, UV coordinates, and tangent.
struct Vertex
{
    glm::vec3 Position{0.0f, 0.0f, 0.0f};
    glm::vec3 Normal{0.0f, 0.0f, 1.0f};
    glm::vec2 TextureCoordinates{0.0f, 0.0f};
    /// @brief Tangent vector in object space. xyz = tangent direction, w = bitangent handedness (+1 or -1).
    glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

/// @brief The most joints one vertex follows. Content that binds a vertex to
///        more keeps its largest weights and is renormalized.
inline constexpr uint32_t kMaxInfluences = 4;

/// @brief Parent index of a joint that has no parent joint.
inline constexpr int32_t kNoParent = -1;

/// @brief How far a vertex's weights may sum from one and still count as
///        normalized; float accumulation over four terms drifts by about this.
inline constexpr float kWeightSumTolerance = 1e-4f;

/// @brief Which joints move one vertex, and how much each does.
///
/// Parallel to MeshData::Vertices. Laid out as the skinning compute pass reads
/// it, so the array uploads as is. A joint whose weight is zero is unused, and
/// names joint 0.
struct VertexSkin
{
    glm::vec4 Weights{1.0f, 0.0f, 0.0f, 0.0f}; ///< Each at least zero; together they sum to one.
    glm::uvec4 Joints{0u, 0u, 0u, 0u};        ///< Indices into the mesh's Skeleton.
};
static_assert(sizeof(VertexSkin) == 32, "VertexSkin is read by the GPU at this size");

/// @brief A joint's transform relative to its parent.
struct JointTransform
{
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Translation{0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f, 1.0f, 1.0f};
};

/// @brief The joints a skinned mesh is bound to, as parallel tables.
///
/// Joints are ordered so every parent comes before its children
/// (`Parents[i] < i`, or kNoParent), which lets a pose be built in one forward
/// pass. A root joint's model-space transform is `RootTransform` times its own
/// rest transform: the nodes above the skeleton stay out of the joint, so an
/// animation that drives the root replaces only the root's own transform.
/// Joints are identified by name, which is how clips find them.
struct Skeleton
{
    glm::mat4 RootTransform{1.0f};          ///< Model-space transform of the root joints' parent.
    std::vector<std::string> Names;         ///< Unique.
    std::vector<int32_t> Parents;           ///< Index of each joint's parent, or kNoParent.
    std::vector<JointTransform> RestLocal;  ///< Each joint's rest transform relative to its parent.
    std::vector<glm::mat4> InverseBind;     ///< Model space to each joint's space at bind time.
    /// Around the bind-pose vertices each joint moves, in model space; empty
    /// (IsEmpty) for a joint that moves none. Fit at import, so the bounds of
    /// any pose come from these and the palette alone.
    std::vector<Aabb> JointBounds;

    [[nodiscard]] uint32_t JointCount() const { return static_cast<uint32_t>(Names.size()); }
    [[nodiscard]] bool Empty() const { return Names.empty(); }
};

/// @brief One drawable index range of a mesh: everything that shares a
///        material slot within one LOD. The unit a draw call submits.
struct SubMesh
{
    uint32_t IndexOffset = 0;  ///< First index in MeshData::Indices.
    uint32_t IndexCount = 0;
    uint32_t MaterialSlot = 0; ///< Index into MeshData::Materials.
    BoundingSphere LocalBounds; ///< Fit over this range at import.
    Aabb LocalAabb;             ///< Fit over this range at import.
};

/// @brief One level of detail: a contiguous run of entries in
///        MeshData::SubMeshes. Submeshes are stored grouped by LOD, LOD0 first.
struct LodRange
{
    uint32_t FirstSubMesh = 0;
    uint32_t SubMeshCount = 0;

    /// @brief Screen-relative height at or above which this level is the one
    ///        drawn: the instance's bounding-sphere diameter over the viewport's
    ///        height. Descends across the chain, so selection takes the first
    ///        level the instance is still big enough for.
    ///
    /// Zero means nothing authored one; DefaultLodScreenSize supplies the value
    /// in that case, so a chain built without thresholds still selects.
    float ScreenSizeThreshold = 0.f;
};

/// @brief The screen-relative height LOD 0 holds down to when the asset carries
///        no authored threshold.
///
/// An eighth of the screen's height, which is further out than it sounds: an
/// instance leaves LOD0 at `radius / (kDefaultLod0ScreenSize * tan(fovY/2))`, so
/// a 0.3 m prop holds full detail to about 4 m and a 5 m building to about 70 m.
/// Set against half the screen — where that prop switched at arm's length and
/// the pop was plain — and against a value low enough that a coarse level is
/// still carrying geometry nobody can see.
inline constexpr float kDefaultLod0ScreenSize = 0.125f;

/// @brief The screen-relative height LOD @p level takes over at when the asset
///        carries no authored threshold, halving per level.
///
/// Halving because that is the ratio a level's triangle budget is authored at —
/// a level meant for half the pixels is where half the triangles belong.
[[nodiscard]] inline constexpr float DefaultLodScreenSize(uint32_t level)
{
    float threshold = kDefaultLod0ScreenSize;
    for (uint32_t i = 0; i < level; ++i)
    {
        threshold *= 0.5f;
    }
    return threshold;
}

/// @brief @p lod's threshold, or the default for @p level when it carries none.
[[nodiscard]] inline constexpr float LodScreenSizeThreshold(const LodRange &lod, uint32_t level)
{
    return lod.ScreenSizeThreshold > 0.f ? lod.ScreenSizeThreshold : DefaultLodScreenSize(level);
}

/// @brief CPU-side mesh: vertices, triangle indices, and the submesh / LOD /
///        material-slot tables that address them.
///
/// Degenerate rule: empty `SubMeshes` means "one implicit submesh spanning the
/// whole index range, material slot 0, engine-fallback material". Factory
/// meshes (DefaultMeshes, prim:// primitives) rely on this — consumers that
/// need explicit tables should normalize via `EnsureSubMeshTables()`.
struct MeshData
{
    std::vector<Vertex> Vertices;
    std::vector<uint32_t>     Indices; ///< Triangle list; every 3 indices form one triangle.
    std::vector<SubMesh>      SubMeshes; ///< May be empty — see degenerate rule above.
    std::vector<LodRange>     Lods;      ///< [0] = LOD0. May be empty alongside SubMeshes.
    std::vector<MaterialData> Materials; ///< Material slot table (import defaults).

    /// Empty for a static mesh. For a skinned one, one entry per vertex, and
    /// the vertices are in bind space rather than baked into the scene.
    std::vector<VertexSkin> Skin;
    Geometry::Skeleton Skeleton; ///< Empty exactly when Skin is.

    /// The collision the model authored as prefixed nodes, which are not part
    /// of the geometry above.
    CollisionData Collision;

    // Whole-mesh bounds, fit over every vertex by EnsureMeshBounds on the import
    // worker, so the main-thread publish reads them instead of re-walking the
    // vertex array. `BoundsComputed` distinguishes "not yet fit" from a
    // legitimately zero-sized mesh.
    BoundingSphere LocalBounds;
    Aabb LocalAabb;
    bool BoundsComputed = false;
};

/// @brief True when `Indices[indexOffset .. indexOffset+indexCount)` is a
///        non-empty range of the index array and every index in it names a
///        vertex — the precondition for dereferencing the range.
inline bool IsFittableIndexRange(const MeshData &meshData, size_t indexOffset, size_t indexCount)
{
    if (indexCount == 0 || indexOffset + indexCount > meshData.Indices.size())
    {
        return false;
    }
    for (size_t i = indexOffset; i < indexOffset + indexCount; ++i)
    {
        if (meshData.Indices[i] >= meshData.Vertices.size())
        {
            return false;
        }
    }
    return true;
}

/// @brief Fits an AABB around the vertices referenced by an index range
///        (`Indices[indexOffset .. indexOffset+indexCount)`) — i.e. a submesh.
///        Out-of-range or empty input, or an index that names no vertex,
///        returns a zero AABB at the origin.
inline Aabb ComputeAabb(const MeshData &meshData, size_t indexOffset, size_t indexCount)
{
    if (!IsFittableIndexRange(meshData, indexOffset, indexCount))
    {
        return {};
    }

    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};
    for (size_t i = indexOffset; i < indexOffset + indexCount; ++i)
    {
        const glm::vec3 &position = meshData.Vertices[meshData.Indices[i]].Position;
        min = glm::min(min, position);
        max = glm::max(max, position);
    }
    return Aabb{.min = min, .max = max};
}

/// @brief Fits an AABB around every vertex of the mesh.
inline Aabb ComputeAabb(const MeshData &meshData)
{
    if (meshData.Vertices.empty())
    {
        return {};
    }

    glm::vec3 min = meshData.Vertices.front().Position;
    glm::vec3 max = min;
    for (const Vertex &vertex : meshData.Vertices)
    {
        min = glm::min(min, vertex.Position);
        max = glm::max(max, vertex.Position);
    }
    return Aabb{.min = min, .max = max};
}

/// @brief Fits a bounding sphere around the vertices referenced by an index
///        range (a submesh). Centre is the range AABB's midpoint; radius is the
///        exact farthest-vertex distance, so the sphere encloses every
///        referenced vertex (never under-culls) while staying tighter than an
///        AABB half-diagonal. Out-of-range or empty input, or an index that
///        names no vertex, returns a zero sphere.
inline BoundingSphere ComputeBoundingSphere(const MeshData &meshData, size_t indexOffset, size_t indexCount)
{
    if (!IsFittableIndexRange(meshData, indexOffset, indexCount))
    {
        return {};
    }

    const Aabb box = ComputeAabb(meshData, indexOffset, indexCount);
    const glm::vec3 center = (box.min + box.max) * 0.5f;

    float radiusSquared = 0.f;
    for (size_t i = indexOffset; i < indexOffset + indexCount; ++i)
    {
        const glm::vec3 offset = meshData.Vertices[meshData.Indices[i]].Position - center;
        radiusSquared = glm::max(radiusSquared, glm::dot(offset, offset));
    }
    return BoundingSphere{.center = center, .radius = std::sqrt(radiusSquared)};
}

/// @brief Fits a bounding sphere around every vertex of the mesh in its local
///        space. Returns a zero-radius sphere at the origin for an empty mesh.
inline BoundingSphere ComputeBoundingSphere(const MeshData &meshData)
{
    if (meshData.Vertices.empty())
    {
        return {};
    }

    const Aabb box = ComputeAabb(meshData);
    const glm::vec3 center = (box.min + box.max) * 0.5f;

    float radiusSquared = 0.f;
    for (const Vertex &vertex : meshData.Vertices)
    {
        const glm::vec3 offset = vertex.Position - center;
        radiusSquared = glm::max(radiusSquared, glm::dot(offset, offset));
    }
    return BoundingSphere{.center = center, .radius = std::sqrt(radiusSquared)};
}

/// @brief Fits the whole-mesh bounds (`LocalBounds` / `LocalAabb`) if they have
///        not been fit yet; a no-op afterwards.
///
/// Walks every vertex three times (ComputeBoundingSphere is itself two passes,
/// plus ComputeAabb), which is expensive on a large mesh: call it wherever a
/// MeshData is produced off the main thread, never on the main thread at
/// publish. Idempotent, so a mesh that arrives already fit costs nothing.
inline void EnsureMeshBounds(MeshData &meshData)
{
    if (meshData.BoundsComputed)
    {
        return;
    }
    meshData.LocalBounds = ComputeBoundingSphere(meshData);
    meshData.LocalAabb = ComputeAabb(meshData);
    meshData.BoundsComputed = true;
}

/// @brief Normalizes the degenerate case in place: a mesh with geometry but no
///        submesh table gains one full-range submesh (slot 0), one LOD spanning
///        it, and — if the slot table is empty — one default-constructed
///        material slot. Meshes with explicit tables are left untouched.
inline void EnsureSubMeshTables(MeshData &meshData)
{
    if (!meshData.SubMeshes.empty() || meshData.Indices.empty())
    {
        return;
    }

    // The implicit whole-mesh submesh spans every vertex, so its bounds ARE the
    // mesh's — share the one fit rather than walking the vertices twice.
    EnsureMeshBounds(meshData);

    SubMesh whole;
    whole.IndexOffset = 0;
    whole.IndexCount = static_cast<uint32_t>(meshData.Indices.size());
    whole.MaterialSlot = 0;
    whole.LocalBounds = meshData.LocalBounds;
    whole.LocalAabb = meshData.LocalAabb;
    meshData.SubMeshes.push_back(whole);

    meshData.Lods.clear();
    meshData.Lods.push_back(
        LodRange{.FirstSubMesh = 0, .SubMeshCount = 1, .ScreenSizeThreshold = DefaultLodScreenSize(0)});

    if (meshData.Materials.empty())
    {
        meshData.Materials.emplace_back();
    }
}

} /* namespace Assisi::Geometry */
