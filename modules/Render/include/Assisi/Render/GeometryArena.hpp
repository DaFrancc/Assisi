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
/// Deliberately deferred: per-mesh
/// Free + semi-space compaction for streaming residency, and a second
/// format-keyed arena for divergent vertex formats. This type is
/// stride-parameterized and standalone precisely so the latter is "instantiate
/// another arena"; neither is built until a caller needs it, but the seams are
/// here so each drops in without touching consumers.

#include <algorithm>
#include <cstdint>
#include <span>

#include <nvrhi/nvrhi.h>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Render
{
/// A skinned mesh's joint weights live in a third buffer beside the vertices,
/// one VertexSkin per vertex, read by the skinning pass. The vertex buffer is
/// also a raw UAV: that pass writes posed vertices into ranges of it, so posed
/// and static geometry bind as one buffer.
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
        uint32_t skinBase    = 0; ///< In VertexSkins; meaningless when skinCount is 0.
        uint32_t skinCount   = 0; ///< 0 for a static mesh, vertexCount for a skinned one.
    };

    /// @brief Where a staging buffer's three parts sit, as MeshBuffer::StageMeshGeometry
    ///        writes them: vertices at 0, then indices, then skins.
    struct StagedLayout
    {
        uint32_t vertexCount = 0;
        uint32_t indexCount  = 0;
        uint32_t skinCount   = 0;
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
        _skinBuffer = nullptr;
        _vertexCapacity = 0;
        _indexCapacity = 0;
        _skinCapacity = 0;
        _vertexUsed = 0;
        _indexUsed = 0;
        _skinUsed = 0;
        EnsureVertexCapacity(initialVertexBytes);
        EnsureIndexCapacity(initialIndexBytes);
        EnsureSkinCapacity(kInitialSkinBytes);
    }

    /// @brief Appends one mesh's joint weights, one per vertex, returning the
    /// first one's index. Recorded into @p sharedList when non-null, as Allocate.
    uint32_t AllocateSkin(std::span<const Geometry::VertexSkin> skins, nvrhi::ICommandList *sharedList = nullptr)
    {
        const uint64_t skinBytes = skins.size_bytes();
        EnsureSkinCapacity(_skinUsed + skinBytes, sharedList);
        const uint32_t skinBase = static_cast<uint32_t>(_skinUsed / sizeof(Geometry::VertexSkin));

        nvrhi::CommandListHandle ownList;
        nvrhi::ICommandList *commandList = sharedList;
        if (commandList == nullptr)
        {
            ownList = _device->createCommandList();
            commandList = ownList;
            commandList->open();
        }
        if (skinBytes > 0)
        {
            commandList->writeBuffer(_skinBuffer, skins.data(), skinBytes, _skinUsed);
        }
        if (ownList != nullptr)
        {
            ownList->close();
            _device->executeCommandList(ownList);
        }
        _skinUsed += skinBytes;
        return skinBase;
    }

    /// @brief Reserves @p vertexCount vertices nothing is written to yet — a
    /// skinned instance's range, which the skinning pass fills every frame.
    /// A grow is recorded into @p commandList, as Allocate.
    uint32_t AllocateVertices(uint32_t vertexCount, nvrhi::ICommandList *commandList)
    {
        const uint64_t vertexBytes = static_cast<uint64_t>(vertexCount) * _vertexStride;
        EnsureVertexCapacity(_vertexUsed + vertexBytes, commandList);
        const uint32_t vertexBase = static_cast<uint32_t>(_vertexUsed / _vertexStride);
        _vertexUsed += vertexBytes;
        return vertexBase;
    }

    /// @brief Appends one mesh's vertices + indices, returning its range. Grows
    /// the backing buffers first if needed. The arena's stride must be
    /// sizeof(Geometry::Vertex).
    ///
    /// When @p sharedList is non-null both the (possible) grow copy and the vertex/
    /// index writes are *recorded* into that already-open command list and NOT
    /// executed — the caller batches many uploads into one submit (see AssetCache's
    /// shared upload list). The grow copy is recorded into the same list *before*
    /// the new-data writes, so linear ordering + nvrhi's auto-barriers keep it
    /// correct, and the old buffer stays alive via the list's referenced resources.
    /// When null, a private command list is created and executed here.
    Range Allocate(std::span<const Geometry::Vertex> vertices, std::span<const uint32_t> indices,
                   nvrhi::ICommandList *sharedList = nullptr)
    {
        ASSISI_ASSERT(_vertexStride == sizeof(Geometry::Vertex), "the arena holds Geometry::Vertex");
        const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
        const uint32_t indexCount = static_cast<uint32_t>(indices.size());
        const uint64_t vertexBytes = vertices.size_bytes();
        const uint64_t indexBytes = indices.size_bytes();

        EnsureVertexCapacity(_vertexUsed + vertexBytes, sharedList);
        EnsureIndexCapacity(_indexUsed + indexBytes, sharedList);

        Range range;
        range.vertexBase = static_cast<uint32_t>(_vertexUsed / _vertexStride);
        range.indexBase = static_cast<uint32_t>(_indexUsed / sizeof(uint32_t));
        range.vertexCount = vertexCount;
        range.indexCount = indexCount;

        nvrhi::CommandListHandle ownList;
        nvrhi::ICommandList *commandList = sharedList;
        if (commandList == nullptr)
        {
            ownList = _device->createCommandList();
            commandList = ownList;
            commandList->open();
        }
        if (vertexBytes > 0)
            commandList->writeBuffer(_vertexBuffer, vertices.data(), vertexBytes, _vertexUsed);
        if (indexBytes > 0)
            commandList->writeBuffer(_indexBuffer, indices.data(), indexBytes, _indexUsed);
        if (ownList != nullptr)
        {
            ownList->close();
            _device->executeCommandList(ownList);
        }

        _vertexUsed += vertexBytes;
        _indexUsed += indexBytes;
        return range;
    }

    /// @brief Appends one mesh whose geometry already lives in a GPU staging
    /// buffer, returning its range. The staged twin of Allocate: instead of
    /// memcpying CPU data into an upload chunk on the calling thread, it records
    /// GPU-side copies out of @p staging — so the per-mesh main-thread cost is two
    /// commands, O(1) in mesh size, rather than O(bytes).
    ///
    /// @p staging holds the vertices at byte offset 0, the indices immediately
    /// after, then the skins (the layout StageMeshGeometry writes, described by
    /// @p layout). It must outlive the submit; the command list's
    /// referenced-resource tracking handles that once recorded, so the caller may
    /// drop its handle right after this returns.
    ///
    /// The worker that filled @p staging never learns its arena offset, so a grow
    /// racing it is impossible by construction — the offsets below are read on the
    /// main thread, after any grow this same call performs.
    Range AllocateStaged(nvrhi::IBuffer *staging, const StagedLayout &layout, nvrhi::ICommandList *commandList)
    {
        const uint64_t vertexBytes = static_cast<uint64_t>(layout.vertexCount) * _vertexStride;
        const uint64_t indexBytes = static_cast<uint64_t>(layout.indexCount) * sizeof(uint32_t);
        const uint64_t skinBytes = static_cast<uint64_t>(layout.skinCount) * sizeof(Geometry::VertexSkin);

        EnsureVertexCapacity(_vertexUsed + vertexBytes, commandList);
        EnsureIndexCapacity(_indexUsed + indexBytes, commandList);
        EnsureSkinCapacity(_skinUsed + skinBytes, commandList);

        Range range;
        range.vertexBase = static_cast<uint32_t>(_vertexUsed / _vertexStride);
        range.indexBase = static_cast<uint32_t>(_indexUsed / sizeof(uint32_t));
        range.vertexCount = layout.vertexCount;
        range.indexCount = layout.indexCount;
        range.skinBase = static_cast<uint32_t>(_skinUsed / sizeof(Geometry::VertexSkin));
        range.skinCount = layout.skinCount;

        // Recorded after the grow copy above, so the destination is the grown
        // buffer and nvrhi's auto-barriers order the two.
        if (vertexBytes > 0)
            commandList->copyBuffer(_vertexBuffer, _vertexUsed, staging, 0, vertexBytes);
        if (indexBytes > 0)
            commandList->copyBuffer(_indexBuffer, _indexUsed, staging, vertexBytes, indexBytes);
        if (skinBytes > 0)
        {
            commandList->copyBuffer(_skinBuffer, _skinUsed, staging, vertexBytes + indexBytes, skinBytes);
        }

        _vertexUsed += vertexBytes;
        _indexUsed += indexBytes;
        _skinUsed += skinBytes;
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
        _skinUsed = 0;
    }

    nvrhi::IBuffer *VertexBuffer() const { return _vertexBuffer; }
    nvrhi::IBuffer *IndexBuffer() const { return _indexBuffer; }
    nvrhi::IBuffer *SkinBuffer() const { return _skinBuffer; }
    uint32_t VertexStride() const { return _vertexStride; }

    /// Occupancy, for the capture's memory counters. Used against capacity says
    /// whether the next Allocate triggers a Grow — a reallocation plus a GPU copy
    /// of the whole prefix, charged to a later frame than the one that caused it.
    [[nodiscard]] uint64_t VertexUsedBytes() const { return _vertexUsed; }
    [[nodiscard]] uint64_t VertexCapacityBytes() const { return _vertexCapacity; }
    [[nodiscard]] uint64_t IndexUsedBytes() const { return _indexUsed; }
    [[nodiscard]] uint64_t IndexCapacityBytes() const { return _indexCapacity; }

private:
    /// Which of the arena's buffers a grow replaces; each is created its own way.
    enum class Part : uint8_t
    {
        Vertices,
        Indices,
        Skins,
    };

    /// What the skin buffer starts at: a few characters' worth of joint weights.
    static constexpr uint64_t kInitialSkinBytes = 1u << 20;

    void EnsureVertexCapacity(uint64_t needed, nvrhi::ICommandList *sharedList = nullptr)
    {
        Grow(_vertexBuffer, _vertexCapacity, _vertexUsed, needed, Part::Vertices, sharedList);
    }
    void EnsureIndexCapacity(uint64_t needed, nvrhi::ICommandList *sharedList = nullptr)
    {
        Grow(_indexBuffer, _indexCapacity, _indexUsed, needed, Part::Indices, sharedList);
    }
    void EnsureSkinCapacity(uint64_t needed, nvrhi::ICommandList *sharedList = nullptr)
    {
        Grow(_skinBuffer, _skinCapacity, _skinUsed, needed, Part::Skins, sharedList);
    }

    /// @brief The description of a @p part buffer of @p byteSize bytes.
    static nvrhi::BufferDesc DescFor(Part part, uint64_t byteSize)
    {
        nvrhi::BufferDesc desc;
        desc.byteSize = byteSize;
        desc.keepInitialState = true;
        switch (part)
        {
        case Part::Vertices:
            desc.isVertexBuffer = true;
            desc.canHaveUAVs = true;
            desc.canHaveRawViews = true;
            desc.initialState = nvrhi::ResourceStates::VertexBuffer;
            desc.debugName = "GeometryArena::Vertex";
            break;
        case Part::Indices:
            desc.isIndexBuffer = true;
            desc.initialState = nvrhi::ResourceStates::IndexBuffer;
            desc.debugName = "GeometryArena::Index";
            break;
        case Part::Skins:
            desc.structStride = sizeof(Geometry::VertexSkin);
            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.debugName = "GeometryArena::Skin";
            break;
        }
        return desc;
    }

    /// @brief Ensures @p buffer holds at least @p needed bytes, reallocating with
    /// geometric growth and GPU-copying the @p used prefix when it must. Holders
    /// that go through VertexBuffer()/IndexBuffer() never see the handle swap. When
    /// @p sharedList is non-null the prefix copy is recorded into it (not executed);
    /// see Allocate.
    void Grow(nvrhi::BufferHandle &buffer, uint64_t &capacity, uint64_t used, uint64_t needed, Part part,
              nvrhi::ICommandList *sharedList = nullptr)
    {
        if (needed <= capacity)
            return;

        const uint64_t newCapacity = std::max(capacity * 2, needed);
        nvrhi::BufferHandle grown = _device->createBuffer(DescFor(part, newCapacity));

        if (used > 0)
        {
            nvrhi::CommandListHandle ownList;
            nvrhi::ICommandList *commandList = sharedList;
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

    nvrhi::IDevice *_device = nullptr;
    uint32_t _vertexStride = 0;
    nvrhi::BufferHandle _vertexBuffer;
    nvrhi::BufferHandle _indexBuffer;
    nvrhi::BufferHandle _skinBuffer;
    uint64_t _vertexCapacity = 0;
    uint64_t _indexCapacity = 0;
    uint64_t _skinCapacity = 0;
    uint64_t _vertexUsed = 0;
    uint64_t _indexUsed = 0;
    uint64_t _skinUsed = 0;
};
} /* namespace Assisi::Render */
