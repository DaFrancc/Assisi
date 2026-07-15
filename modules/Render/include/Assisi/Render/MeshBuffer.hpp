/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshBuffer.hpp
/// @brief GPU-side mesh storage backed by NVRHI vertex/index buffers.
///
/// Upload CPU-side `MeshData` once; NVRHI buffer handles are reference-counted,
/// so no manual destructor is needed. Copyable and movable.
///
/// The CPU vertex/index data is NOT retained after upload — bounds live in the
/// submesh table, and any future CPU consumer (mesh colliders, editor picking)
/// re-imports or opts in at resolve time (see
/// docs/mesh-material-architecture.md §1, §8).

#include <cstdint>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Render
{
/// @brief Owner of an NVRHI vertex + index buffer pair for a single mesh,
///        plus the submesh / LOD / material-slot tables that address it.
///
/// Vertex layout matches `Assisi::Geometry::Vertex`: Position, Normal,
/// TextureCoordinates, Tangent. Draw submission addresses geometry as
/// `(MeshBuffer, submeshIndex)` — a submesh is an index range into the shared
/// index buffer. When the shared geometry arena lands (GPU-driven stage 2),
/// this class becomes a range record into arena buffers; submesh offsets are
/// already relative, so consumers don't change.
class MeshBuffer
{
  public:
    MeshBuffer() = default;

    /// @brief Uploads mesh data to the GPU immediately.
    MeshBuffer(nvrhi::IDevice *device, Geometry::MeshData meshData) { Upload(device, std::move(meshData)); }

    /// @brief Creates new GPU buffers and uploads `meshData` into them, replacing
    /// any buffers already owned by this instance.
    ///
    /// Normalizes the degenerate case first (a mesh with no submesh table gains
    /// one full-range submesh — factory/primitive meshes rely on this), then
    /// keeps the submesh/LOD/material tables and drops the vertex/index data.
    void Upload(nvrhi::IDevice *device, Geometry::MeshData meshData)
    {
        Geometry::EnsureSubMeshTables(meshData);

        _indexCount = static_cast<uint32_t>(meshData.Indices.size());

        nvrhi::BufferDesc vertexDesc;
        vertexDesc.byteSize = meshData.Vertices.size() * sizeof(Geometry::Vertex);
        vertexDesc.isVertexBuffer = true;
        vertexDesc.debugName = "MeshBuffer::VertexBuffer";
        vertexDesc.initialState = nvrhi::ResourceStates::VertexBuffer;
        vertexDesc.keepInitialState = true;
        _vertexBuffer = device->createBuffer(vertexDesc);

        nvrhi::BufferDesc indexDesc;
        indexDesc.byteSize = meshData.Indices.size() * sizeof(unsigned int);
        indexDesc.isIndexBuffer = true;
        indexDesc.debugName = "MeshBuffer::IndexBuffer";
        indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
        indexDesc.keepInitialState = true;
        _indexBuffer = device->createBuffer(indexDesc);

        nvrhi::CommandListHandle commandList = device->createCommandList();
        commandList->open();
        commandList->writeBuffer(_vertexBuffer, meshData.Vertices.data(), vertexDesc.byteSize);
        commandList->writeBuffer(_indexBuffer, meshData.Indices.data(), indexDesc.byteSize);
        commandList->close();
        device->executeCommandList(commandList);

        _localBounds = Geometry::ComputeBoundingSphere(meshData);
        _localAabb = Geometry::ComputeAabb(meshData);
        _subMeshes = std::move(meshData.SubMeshes);
        _lods = std::move(meshData.Lods);
        _materials = std::move(meshData.Materials);
        // Vertex/index data is deliberately not retained (see file comment).
    }

    nvrhi::IBuffer *VertexBuffer() const { return _vertexBuffer; }
    nvrhi::IBuffer *IndexBuffer() const { return _indexBuffer; }
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
    nvrhi::BufferHandle _vertexBuffer;
    nvrhi::BufferHandle _indexBuffer;
    uint32_t             _indexCount = 0;
    uint32_t             _id = 0; ///< Assigned by AssetCache; 0 until then.
    Geometry::BoundingSphere _localBounds;
    Geometry::Aabb           _localAabb;
    std::vector<Geometry::SubMesh>      _subMeshes;
    std::vector<Geometry::LodRange>     _lods;
    std::vector<Geometry::MaterialData> _materials;
};
} /* namespace Assisi::Render */
