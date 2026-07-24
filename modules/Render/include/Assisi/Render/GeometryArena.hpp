/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GeometryArena.hpp
/// @brief One shared vertex + index buffer that many meshes sub-allocate ranges
///        from (GPU-driven stage C). Replaces per-mesh buffers so the whole
///        scene's geometry binds once and draws vary only by base offset — the
///        precondition for indirect draws (stages E/F).
///
/// Allocation is a growable **bump allocator**: meshes append; Reset() frees
/// everything at once (matches AssetCache::Clear). When a mesh doesn't fit, the
/// backing buffer is reallocated with geometric growth and the existing prefix
/// is GPU-copied over — offsets are unchanged, only the handle swaps. Because a
/// MeshBuffer indirects through its arena rather than caching a raw handle, the
/// swap is invisible to the draw loop.
///
/// Deliberately deferred (see docs/asset-streaming-design-notes.md): per-mesh
/// Free + semi-space compaction for streaming residency, and a second
/// format-keyed arena for divergent vertex formats. This type is
/// stride-parameterized and standalone precisely so the latter is "instantiate
/// another arena"; neither is built until a caller needs it, but the seams are
/// here so each drops in without touching consumers.

#include <algorithm>
#include <cstdint>

#include <nvrhi/nvrhi.h>

namespace Assisi::Render
{
class GeometryArena
{
  public:
    /// @brief A mesh's sub-allocation: base offsets into the shared buffers.
    /// `vertexBase` feeds drawIndexed's startVertexLocation (baseVertex, added to
    /// every index); `indexBase` + a submesh's IndexOffset feeds startIndexLocation.
    /// Index values stay mesh-local, so nothing is rewritten on upload.
    struct Range
    {
        uint32_t vertexBase  = 0; ///< In vertices.
        uint32_t indexBase   = 0; ///< In indices.
        uint32_t vertexCount = 0;
        uint32_t indexCount  = 0;
    };

    GeometryArena() = default;

    /// @brief Bind to a device and fix the vertex stride (the arena's single
    /// format). @p initialVertexBytes / @p initialIndexBytes are starting
    /// capacities; the buffers grow geometrically past them on demand.
    void Initialize(nvrhi::IDevice *device, uint32_t vertexStride, uint64_t initialVertexBytes = (4u << 20),
                    uint64_t initialIndexBytes = (2u << 20))
    {
        _device = device;
        _vertexStride = vertexStride;
        _vertexBuffer = nullptr;
        _indexBuffer = nullptr;
        _vertexCapacity = 0;
        _indexCapacity = 0;
        _vertexUsed = 0;
        _indexUsed = 0;
        EnsureVertexCapacity(initialVertexBytes);
        EnsureIndexCapacity(initialIndexBytes);
    }

    /// @brief Appends one mesh's vertices + indices, returning its range. Grows
    /// the backing buffers first if needed. @p vertexData is @p vertexCount
    /// vertices of the arena's stride; @p indexData is @p indexCount 32-bit indices.
    ///
    /// When @p sharedList is non-null both the (possible) grow copy and the vertex/
    /// index writes are *recorded* into that already-open command list and NOT
    /// executed — the caller batches many uploads into one submit (see AssetCache's
    /// shared upload list). The grow copy is recorded into the same list *before*
    /// the new-data writes, so linear ordering + nvrhi's auto-barriers keep it
    /// correct, and the old buffer stays alive via the list's referenced resources.
    /// When null, a private command list is created and executed here as before.
    Range Allocate(const void *vertexData, uint32_t vertexCount, const uint32_t *indexData, uint32_t indexCount,
                   nvrhi::ICommandList *sharedList = nullptr)
    {
        const uint64_t vertexBytes = static_cast<uint64_t>(vertexCount) * _vertexStride;
        const uint64_t indexBytes = static_cast<uint64_t>(indexCount) * sizeof(uint32_t);

        EnsureVertexCapacity(_vertexUsed + vertexBytes, sharedList);
        EnsureIndexCapacity(_indexUsed + indexBytes, sharedList);

        Range range;
        range.vertexBase = static_cast<uint32_t>(_vertexUsed / _vertexStride);
        range.indexBase = static_cast<uint32_t>(_indexUsed / sizeof(uint32_t));
        range.vertexCount = vertexCount;
        range.indexCount = indexCount;

        nvrhi::CommandListHandle ownList;
        nvrhi::ICommandList     *commandList = sharedList;
        if (commandList == nullptr)
        {
            ownList = _device->createCommandList();
            commandList = ownList;
            commandList->open();
        }
        if (vertexBytes > 0)
            commandList->writeBuffer(_vertexBuffer, vertexData, vertexBytes, _vertexUsed);
        if (indexBytes > 0)
            commandList->writeBuffer(_indexBuffer, indexData, indexBytes, _indexUsed);
        if (ownList != nullptr)
        {
            ownList->close();
            _device->executeCommandList(ownList);
        }

        _vertexUsed += vertexBytes;
        _indexUsed += indexBytes;
        return range;
    }

    /// @brief Frees every range at once (bump cursor back to 0), keeping the
    /// buffers for reuse — the wholesale free that matches AssetCache::Clear.
    /// Per-mesh Free + compaction is the streaming-era extension (see the
    /// streaming design notes); it slots in here without changing consumers.
    void Reset()
    {
        _vertexUsed = 0;
        _indexUsed = 0;
    }

    nvrhi::IBuffer *VertexBuffer() const { return _vertexBuffer; }
    nvrhi::IBuffer *IndexBuffer() const { return _indexBuffer; }
    uint32_t VertexStride() const { return _vertexStride; }

  private:
    void EnsureVertexCapacity(uint64_t needed, nvrhi::ICommandList *sharedList = nullptr)
    {
        Grow(_vertexBuffer, _vertexCapacity, _vertexUsed, needed, true, sharedList);
    }
    void EnsureIndexCapacity(uint64_t needed, nvrhi::ICommandList *sharedList = nullptr)
    {
        Grow(_indexBuffer, _indexCapacity, _indexUsed, needed, false, sharedList);
    }

    /// @brief Ensures @p buffer holds at least @p needed bytes, reallocating with
    /// geometric growth and GPU-copying the @p used prefix when it must. Holders
    /// that go through VertexBuffer()/IndexBuffer() never see the handle swap. When
    /// @p sharedList is non-null the prefix copy is recorded into it (not executed);
    /// see Allocate.
    void Grow(nvrhi::BufferHandle &buffer, uint64_t &capacity, uint64_t used, uint64_t needed, bool isVertex,
              nvrhi::ICommandList *sharedList = nullptr)
    {
        if (needed <= capacity)
            return;

        const uint64_t newCapacity = std::max(capacity * 2, needed);

        nvrhi::BufferDesc desc;
        desc.byteSize = newCapacity;
        desc.isVertexBuffer = isVertex;
        desc.isIndexBuffer = !isVertex;
        desc.initialState = isVertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;
        desc.keepInitialState = true;
        desc.debugName = isVertex ? "GeometryArena::Vertex" : "GeometryArena::Index";
        nvrhi::BufferHandle grown = _device->createBuffer(desc);

        if (used > 0)
        {
            nvrhi::CommandListHandle ownList;
            nvrhi::ICommandList     *commandList = sharedList;
            if (commandList == nullptr)
            {
                ownList = _device->createCommandList();
                commandList = ownList;
                commandList->open();
            }
            commandList->copyBuffer(grown, 0, buffer, 0, used);
            if (ownList != nullptr)
            {
                ownList->close();
                _device->executeCommandList(ownList);
            }
        }

        buffer = grown; // old handle released by ref-count once its last use retires
        capacity = newCapacity;
    }

    nvrhi::IDevice     *_device = nullptr;
    uint32_t            _vertexStride = 0;
    nvrhi::BufferHandle _vertexBuffer;
    nvrhi::BufferHandle _indexBuffer;
    uint64_t            _vertexCapacity = 0;
    uint64_t            _indexCapacity = 0;
    uint64_t            _vertexUsed = 0;
    uint64_t            _indexUsed = 0;
};
} /* namespace Assisi::Render */
