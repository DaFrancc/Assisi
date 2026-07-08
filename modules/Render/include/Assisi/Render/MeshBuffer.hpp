/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshBuffer.hpp
/// @brief GPU-side mesh storage backed by NVRHI vertex/index buffers.
///
/// Upload CPU-side `MeshData` once; NVRHI buffer handles are reference-counted,
/// so no manual destructor is needed. Copyable and movable.

#include <cstdint>

#include <nvrhi/nvrhi.h>

#include <Assisi/Render/MeshData.hpp>

namespace Assisi::Render
{
/// @brief Owner of an NVRHI vertex + index buffer pair for a single mesh.
///
/// Vertex layout matches `Assisi::Render::Vertex`: Position, Normal,
/// TextureCoordinates, Tangent.
class MeshBuffer
{
  public:
    MeshBuffer() = default;

    /// @brief Uploads mesh data to the GPU immediately.
    MeshBuffer(nvrhi::IDevice *device, MeshData meshData) { Upload(device, std::move(meshData)); }

    /// @brief Creates new GPU buffers and uploads `meshData` into them, replacing
    /// any buffers already owned by this instance.
    void Upload(nvrhi::IDevice *device, MeshData meshData)
    {
        _indexCount = static_cast<uint32_t>(meshData.Indices.size());

        nvrhi::BufferDesc vertexDesc;
        vertexDesc.byteSize = meshData.Vertices.size() * sizeof(Vertex);
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

        _sourceData = std::move(meshData);
    }

    nvrhi::IBuffer *VertexBuffer() const { return _vertexBuffer; }
    nvrhi::IBuffer *IndexBuffer() const { return _indexBuffer; }
    uint32_t IndexCount() const { return _indexCount; }

    /// @brief CPU-side geometry this buffer was last uploaded from (e.g. for physics).
    const MeshData &SourceData() const { return _sourceData; }

  private:
    nvrhi::BufferHandle _vertexBuffer;
    nvrhi::BufferHandle _indexBuffer;
    uint32_t             _indexCount = 0;
    MeshData              _sourceData;
};
} /* namespace Assisi::Render */
