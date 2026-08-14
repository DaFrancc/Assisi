/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshBuffer.hpp
/// @brief A mesh's range record into a shared GeometryArena, plus the
///        submesh / LOD / material-slot tables that address it (GPU-driven
///        stage C).
///
/// Upload CPU-side `MeshData` once into an arena; the arena owns the GPU
/// buffers, this class just records where the mesh's vertices/indices landed
/// (base offsets) and which arena holds them. Copyable and movable — it is a
/// lightweight handle, not a buffer owner.
///
/// The CPU vertex/index data is NOT retained after upload — bounds live in the
/// submesh table, and any future CPU consumer (mesh colliders, editor picking)
/// re-imports or opts in at resolve time (see
/// docs/mesh-material-architecture.md §1, §8).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Render/GeometryArena.hpp>

namespace Assisi::Render
{
/// @brief Owner of an NVRHI vertex + index buffer pair for a single mesh,
///        plus the submesh / LOD / material-slot tables that address it.
///
/// Vertex layout matches `Assisi::Geometry::Vertex`: Position, Normal,
/// TextureCoordinates, Tangent. Draw submission addresses geometry as
/// `(MeshBuffer, submeshIndex)` — a submesh is an index range into the arena's
/// shared index buffer, offset by this mesh's arena base (see VertexBase /
/// IndexBase). Submesh offsets stay mesh-relative, so consumers add the base at
/// draw time and never rewrite index data.
class MeshBuffer
{
public:
    MeshBuffer() = default;

    /// @brief Sub-allocates a range in @p arena and uploads `meshData` into it.
    ///
    /// Normalizes the degenerate case first (a mesh with no submesh table gains
    /// one full-range submesh — factory/primitive meshes rely on this), records
    /// the arena and the returned base offsets, then keeps the
    /// submesh/LOD/material tables and drops the vertex/index data. The arena
    /// owns the GPU buffers; this instance only references them.
    /// @p sharedList, when non-null, batches this mesh's arena upload into the
    /// caller's shared command list (recorded, not executed here — see
    /// GeometryArena::Allocate). Null keeps the self-contained synchronous upload.
    void Upload(GeometryArena &arena, Geometry::MeshData meshData, nvrhi::ICommandList *sharedList = nullptr)
    {
        Geometry::EnsureSubMeshTables(meshData);

        _indexCount = static_cast<uint32_t>(meshData.Indices.size());

        const GeometryArena::Range range =
            arena.Allocate(meshData.Vertices.data(), static_cast<uint32_t>(meshData.Vertices.size()),
                           meshData.Indices.data(), static_cast<uint32_t>(meshData.Indices.size()), sharedList);
        _arena = &arena;
        _vertexBase = range.vertexBase;
        _indexBase = range.indexBase;

        // Read the bounds the import worker already fit (EnsureMeshBounds) rather
        // than re-walking the vertex array here — three passes over a large mesh is
        // milliseconds on the main thread mid-frame. The call is a no-op for an
        // imported mesh; it only computes for meshes built in-process (prim://
        // factories, tests), which are tiny.
        Geometry::EnsureMeshBounds(meshData);
        _localBounds = meshData.LocalBounds;
        _localAabb = meshData.LocalAabb;
        _subMeshes = std::move(meshData.SubMeshes);
        _lods = std::move(meshData.Lods);
        _materials = std::move(meshData.Materials);
        // Vertex/index data is deliberately not retained (see file comment).
    }

    /// @brief Copy a mesh's geometry into @p staging — the worker-thread half of a
    /// staged upload. @p staging must be at least MeshStagingBytes(meshData) big and
    /// CPU-writable; the caller supplies it (pooled, so streaming does not churn GPU
    /// allocations). Returns false if the mesh has no geometry or the map fails.
    ///
    /// This is where the bulk memcpy happens, and the whole point is that it runs on
    /// a decode/import worker instead of the main thread: `writeBuffer`'s staging
    /// copy would otherwise run on whichever thread records it, making the
    /// main-thread publish O(mesh bytes). Free-threaded — `mapBuffer` on a buffer
    /// whose previous submit has retired takes no GPU wait.
    ///
    /// Layout: vertices at offset 0, then indices — what
    /// GeometryArena::AllocateStaged expects. nvrhi rejects `writeBuffer` on a
    /// mappable buffer, so the fill goes through map + memcpy + unmap by necessity.
    static bool StageMeshGeometry(nvrhi::IDevice *device, nvrhi::IBuffer *staging,
                                  const Geometry::MeshData &meshData)
    {
        const uint64_t vertexBytes = MeshVertexBytes(meshData);
        const uint64_t indexBytes = MeshIndexBytes(meshData);
        if (staging == nullptr || vertexBytes + indexBytes == 0)
        {
            return false;
        }

        void *mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Write);
        if (mapped == nullptr)
        {
            return false;
        }
        if (vertexBytes > 0)
        {
            std::memcpy(mapped, meshData.Vertices.data(), vertexBytes);
        }
        if (indexBytes > 0)
        {
            std::memcpy(static_cast<std::byte *>(mapped) + vertexBytes, meshData.Indices.data(), indexBytes);
        }
        device->unmapBuffer(staging);
        return true;
    }

    static uint64_t MeshVertexBytes(const Geometry::MeshData &meshData)
    {
        return meshData.Vertices.size() * sizeof(Geometry::Vertex);
    }
    static uint64_t MeshIndexBytes(const Geometry::MeshData &meshData)
    {
        return meshData.Indices.size() * sizeof(uint32_t);
    }
    /// @brief Staging-buffer size a staged upload of @p meshData needs.
    static uint64_t MeshStagingBytes(const Geometry::MeshData &meshData)
    {
        return MeshVertexBytes(meshData) + MeshIndexBytes(meshData);
    }

    /// @brief Upload from a staging buffer the worker already filled (see
    /// StageMeshGeometry) — the main-thread half. Identical bookkeeping to
    /// Upload(), but the geometry moves GPU-side via recorded copies instead of a
    /// main-thread memcpy, so this is O(1) in mesh size.
    ///
    /// @p meshData supplies only the metadata (submesh/LOD/material tables, bounds,
    /// index count); its Vertices/Indices may already have been released by the
    /// worker once staged.
    void UploadStaged(GeometryArena &arena, Geometry::MeshData meshData, nvrhi::IBuffer *staging,
                      uint32_t vertexCount, uint32_t indexCount, nvrhi::ICommandList *commandList)
    {
        _indexCount = indexCount;

        const GeometryArena::Range range = arena.AllocateStaged(staging, vertexCount, indexCount, commandList);
        _arena = &arena;
        _vertexBase = range.vertexBase;
        _indexBase = range.indexBase;

        // Bounds were fit on the import worker (EnsureMeshBounds); the vertex data
        // may be gone by now, so they must already be present — unlike Upload(),
        // there is nothing here to recompute them from.
        _localBounds = meshData.LocalBounds;
        _localAabb = meshData.LocalAabb;
        _subMeshes = std::move(meshData.SubMeshes);
        _lods = std::move(meshData.Lods);
        _materials = std::move(meshData.Materials);
    }

    /// @brief The arena's shared vertex/index buffers (null until Upload). Read
    /// fresh each frame so an arena grow/compaction that swaps the handle is
    /// transparent to the draw loop.
    nvrhi::IBuffer *VertexBuffer() const { return _arena != nullptr ? _arena->VertexBuffer() : nullptr; }
    nvrhi::IBuffer *IndexBuffer() const { return _arena != nullptr ? _arena->IndexBuffer() : nullptr; }

    /// @brief This mesh's base offset into the arena, in vertices — drawIndexed's
    /// startVertexLocation (added to every mesh-local index).
    uint32_t VertexBase() const { return _vertexBase; }
    /// @brief This mesh's base offset into the arena, in indices — added to a
    /// submesh's IndexOffset to form startIndexLocation.
    uint32_t IndexBase() const { return _indexBase; }
    uint32_t IndexCount() const { return _indexCount; }

    /// @brief Stable, process-unique id assigned by AssetCache at upload (never
    /// reused; survives Clear()). 0 = unassigned. Symmetric with Material::Id —
    /// it keys the draw sort by mesh and becomes the arena mesh index later
    /// (docs/mesh-material-architecture.md §1). Set once by the cache; the
    /// MeshBuffer itself never mints.
    uint32_t Id() const { return _id; }
    void SetId(uint32_t id) { _id = id; }

    /// @brief Local-space bounding sphere over the whole mesh, for frustum culling.
    const Geometry::BoundingSphere &LocalBounds() const { return _localBounds; }

    /// @brief Local-space AABB over the whole mesh (GPU-driven stage 1's cull refine).
    const Geometry::Aabb &LocalAabb() const { return _localAabb; }

    /// @brief Drawable index ranges, grouped by LOD (LOD0 first). Never empty
    ///        after a successful Upload of non-empty geometry.
    const std::vector<Geometry::SubMesh> &SubMeshes() const { return _subMeshes; }

    /// @brief LOD table addressing SubMeshes(); [0] = LOD0.
    const std::vector<Geometry::LodRange> &Lods() const { return _lods; }

    /// @brief Material slot table carried from import (the mesh's default
    ///        materials). Indexed by SubMesh::MaterialSlot.
    const std::vector<Geometry::MaterialData> &Materials() const { return _materials; }

private:
    // The arena that owns this mesh's geometry, and where the mesh landed in it.
    // A pointer (not a raw buffer handle) so an arena grow/compaction that swaps
    // the underlying buffer is picked up automatically. Null until Upload().
    const GeometryArena *_arena = nullptr;
    uint32_t _vertexBase = 0;             ///< Base offset into the arena, in vertices.
    uint32_t _indexBase = 0;              ///< Base offset into the arena, in indices.
    uint32_t _indexCount = 0;
    uint32_t _id = 0;             ///< Assigned by AssetCache; 0 until then.
    Geometry::BoundingSphere _localBounds;
    Geometry::Aabb _localAabb;
    std::vector<Geometry::SubMesh>      _subMeshes;
    std::vector<Geometry::LodRange>     _lods;
    std::vector<Geometry::MaterialData> _materials;
};
} /* namespace Assisi::Render */
