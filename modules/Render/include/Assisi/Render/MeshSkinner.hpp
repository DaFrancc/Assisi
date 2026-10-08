/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshSkinner.hpp
/// @brief The compute pre-pass that poses skinned instances: skin.comp reads a
///        mesh's bind-pose vertices from the geometry arena and writes each
///        instance's posed copy to its own range of the same arena.
///
/// Posed vertices are ordinary arena vertices, so the cull pass, the single
/// indirect draw and the shadow passes draw them as they would any static mesh;
/// only the base vertex differs per instance.
///
/// Two halves, split so the packing is testable without a device:
///   - `SkinBatch` (pure): the frame's dispatch records and every instance's
///     palette, packed end to end.
///   - `MeshSkinner` (device): uploads the palettes and records one dispatch per
///     record.

#include <cstdint>
#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/ComputeShader.hpp>

namespace Assisi::Render
{

/// @brief One instance's dispatch: where its bind-pose vertices and skins are,
///        where its posed vertices go, and where its palette starts. Pushed as
///        skin.comp's push-constant block.
struct SkinDispatch
{
    uint32_t sourceVertexBase = 0; ///< The mesh's bind-pose vertices, in arena vertices.
    uint32_t posedVertexBase = 0;  ///< The instance's own range, in arena vertices.
    uint32_t vertexCount = 0;
    uint32_t skinBase = 0;    ///< The mesh's first VertexSkin in the arena's skin buffer.
    uint32_t paletteBase = 0; ///< The instance's first joint matrix in the frame's palette buffer.
};
static_assert(sizeof(SkinDispatch) == 20, "SkinDispatch must match skin.comp's push_constant block.");

/// @brief The frame's dispatches and the palettes they read.
class SkinBatch
{
public:
    /// @brief Drops the previous frame's records.
    void Reset();

    /// @brief Appends one instance, copying @p palette after every palette added
    ///        before it and pointing @p dispatch's paletteBase at it. Ignored
    ///        when it has no vertices.
    void Add(SkinDispatch dispatch, std::span<const glm::mat4> palette);

    [[nodiscard]] const std::vector<SkinDispatch> &Dispatches() const { return _dispatches; }
    [[nodiscard]] const std::vector<glm::mat4> &Palettes() const { return _palettes; }
    [[nodiscard]] bool Empty() const { return _dispatches.empty(); }

private:
    std::vector<SkinDispatch> _dispatches;
    std::vector<glm::mat4> _palettes;
};

/// @brief Owns skin.comp's pipeline and the palette buffer, and records a
///        frame's dispatches.
class MeshSkinner
{
public:
    /// @brief Loads skin.comp. False if its pipeline failed to build; posed
    ///        instances then keep whatever their range last held.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device);

    /// @brief Uploads @p batch's palettes and records one dispatch per record,
    ///        reading and writing @p vertexBuffer and reading @p skinBuffer.
    ///        No-op for an empty batch, or before the arena has both buffers.
    void Dispatch(nvrhi::ICommandList *commandList, const SkinBatch &batch, nvrhi::IBuffer *vertexBuffer,
                  nvrhi::IBuffer *skinBuffer);

    [[nodiscard]] bool IsValid() const { return _shader.IsValid(); }

private:
    /// @brief Grows the palette buffer to @p neededMatrices if it is smaller.
    void EnsurePaletteCapacity(uint32_t neededMatrices);
    /// @brief (Re)builds the binding set when any buffer it names changed.
    void RebindIfChanged(nvrhi::IBuffer *vertexBuffer, nvrhi::IBuffer *skinBuffer);

    ComputeShader _shader;
    Buffer _paletteBuffer;
    nvrhi::BindingSetHandle _bindingSet;
    nvrhi::IDevice *_device = nullptr;
    // What the binding set was built against: an arena grow swaps a handle.
    nvrhi::IBuffer *_boundVertices = nullptr;
    nvrhi::IBuffer *_boundSkins = nullptr;
    nvrhi::IBuffer *_boundPalettes = nullptr;
};

} // namespace Assisi::Render
