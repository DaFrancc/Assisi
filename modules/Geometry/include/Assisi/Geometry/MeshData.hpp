/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshData.hpp
/// @brief CPU-side mesh representation — the decode target for mesh importers
///        and the input to Render's MeshBuffer::Upload().
///
/// This lives in Geometry (not Render) on purpose: it is pure CPU data with no
/// GPU dependency, so importers, physics, tools, and tests can produce or read
/// geometry without linking the renderer.
///
/// One vertex array + one index array per mesh asset, addressed through
/// SubMesh index ranges. The storage shape is deliberately flat so it later
/// relocates into a shared geometry arena (GPU-driven stage 2) by adding base
/// offsets — SubMesh offsets are already relative, so consumers never change.
/// See docs/mesh-material-architecture.md §1.

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <Assisi/Geometry/Bounds.hpp>
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

/// @brief One drawable index range of a mesh: everything that shares a
///        material slot within one LOD. The unit a draw call submits.
struct SubMesh
{
    uint32_t IndexOffset = 0;  ///< First index in MeshData::Indices.
    uint32_t IndexCount = 0;
    uint32_t MaterialSlot = 0; ///< Index into MeshData::Materials.
    BoundingSphere LocalBounds; ///< Fit over this range at import.
    Aabb           LocalAabb;   ///< Fit over this range at import.
};

/// @brief One level of detail: a contiguous run of entries in
///        MeshData::SubMeshes. Submeshes are stored grouped by LOD, LOD0 first.
struct LodRange
{
    uint32_t FirstSubMesh = 0;
    uint32_t SubMeshCount = 0;
};

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
};

/// @brief Fits an AABB around the vertices referenced by an index range
///        (`Indices[indexOffset .. indexOffset+indexCount)`) — i.e. a submesh.
///        Out-of-range or empty input returns a zero AABB at the origin.
inline Aabb ComputeAabb(const MeshData &meshData, size_t indexOffset, size_t indexCount)
{
    if (indexCount == 0 || indexOffset + indexCount > meshData.Indices.size())
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
///        AABB half-diagonal. Out-of-range or empty input returns a zero sphere.
inline BoundingSphere ComputeBoundingSphere(const MeshData &meshData, size_t indexOffset, size_t indexCount)
{
    if (indexCount == 0 || indexOffset + indexCount > meshData.Indices.size())
    {
        return {};
    }

    const Aabb      box = ComputeAabb(meshData, indexOffset, indexCount);
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

    const Aabb      box = ComputeAabb(meshData);
    const glm::vec3 center = (box.min + box.max) * 0.5f;

    float radiusSquared = 0.f;
    for (const Vertex &vertex : meshData.Vertices)
    {
        const glm::vec3 offset = vertex.Position - center;
        radiusSquared = glm::max(radiusSquared, glm::dot(offset, offset));
    }
    return BoundingSphere{.center = center, .radius = std::sqrt(radiusSquared)};
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

    SubMesh whole;
    whole.IndexOffset = 0;
    whole.IndexCount = static_cast<uint32_t>(meshData.Indices.size());
    whole.MaterialSlot = 0;
    whole.LocalBounds = ComputeBoundingSphere(meshData);
    whole.LocalAabb = ComputeAabb(meshData);
    meshData.SubMeshes.push_back(whole);

    meshData.Lods.clear();
    meshData.Lods.push_back(LodRange{.FirstSubMesh = 0, .SubMeshCount = 1});

    if (meshData.Materials.empty())
    {
        meshData.Materials.emplace_back();
    }
}

} /* namespace Assisi::Geometry */
