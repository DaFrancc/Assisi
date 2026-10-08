/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Render/MeshSkinner.hpp>

#include <algorithm>
#include <utility>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/MeshData.hpp>

namespace Assisi::Render
{

namespace
{

// Where skin.comp's buffers sit in their register space; each must match its
// layout(binding = …). UAVs are offset into their own range by the backend.
enum class ReadSlot : std::uint8_t
{
    Skins = 0,
    Palettes = 1,
};

enum class WriteSlot : std::uint8_t
{
    Vertices = 0,
};

// Threads per workgroup, one per vertex. Must equal skin.comp's local_size_x.
constexpr uint32_t kSkinWorkgroupSize = 64u;

// Joint matrices the palette buffer starts with: a few characters' worth.
constexpr uint32_t kInitialPaletteMatrices = 1024u;

static_assert(sizeof(Geometry::VertexSkin) == 32, "skin.comp reads VertexSkin as {vec4 weights; uvec4 joints;}");
static_assert(sizeof(Geometry::Vertex) == 48, "skin.comp addresses a vertex as twelve 32-bit words");

} // namespace

void SkinBatch::Reset()
{
    _dispatches.clear();
    _palettes.clear();
}

void SkinBatch::Add(SkinDispatch dispatch, std::span<const glm::mat4> palette)
{
    if (dispatch.vertexCount == 0)
    {
        return;
    }
    dispatch.paletteBase = static_cast<uint32_t>(_palettes.size());
    _palettes.insert(_palettes.end(), palette.begin(), palette.end());
    _dispatches.push_back(dispatch);
}

bool MeshSkinner::Initialize(nvrhi::IDevice *device)
{
    _device = device;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ReadSlot::Skins)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ReadSlot::Palettes)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RawBuffer_UAV(std::to_underlying(WriteSlot::Vertices)));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(SkinDispatch)));
    if (!_shader.Initialize(device, "shaders/skin.comp.spv", layoutDesc))
    {
        Core::Log::Error("MeshSkinner: failed to build the skin compute pipeline.");
        return false;
    }
    EnsurePaletteCapacity(kInitialPaletteMatrices);
    return true;
}

void MeshSkinner::EnsurePaletteCapacity(uint32_t neededMatrices)
{
    if (_paletteBuffer.IsValid() && neededMatrices <= _paletteBuffer.CapacityElements())
    {
        return;
    }
    const uint32_t capacity = std::max(_paletteBuffer.CapacityElements() * 2u, neededMatrices);
    _paletteBuffer.Create(_device, sizeof(glm::mat4), capacity, /*allowUnorderedAccess=*/ false,
                          "MeshSkinner::Palettes");
}

void MeshSkinner::RebindIfChanged(nvrhi::IBuffer *vertexBuffer, nvrhi::IBuffer *skinBuffer)
{
    nvrhi::IBuffer *palettes = _paletteBuffer.NativeBuffer();
    if (_bindingSet != nullptr && vertexBuffer == _boundVertices && skinBuffer == _boundSkins &&
        palettes == _boundPalettes)
    {
        return;
    }
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ReadSlot::Skins), skinBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ReadSlot::Palettes), palettes));
    setDesc.addItem(nvrhi::BindingSetItem::RawBuffer_UAV(std::to_underlying(WriteSlot::Vertices), vertexBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(SkinDispatch)));
    _bindingSet = _device->createBindingSet(setDesc, _shader.BindingLayout());
    _boundVertices = vertexBuffer;
    _boundSkins = skinBuffer;
    _boundPalettes = palettes;
}

void MeshSkinner::Dispatch(nvrhi::ICommandList *commandList, const SkinBatch &batch, nvrhi::IBuffer *vertexBuffer,
                           nvrhi::IBuffer *skinBuffer)
{
    if (!IsValid() || batch.Empty() || vertexBuffer == nullptr || skinBuffer == nullptr)
    {
        return;
    }
    const uint32_t matrixCount = static_cast<uint32_t>(batch.Palettes().size());
    EnsurePaletteCapacity(matrixCount);
    RebindIfChanged(vertexBuffer, skinBuffer);
    if (_bindingSet == nullptr)
    {
        return;
    }
    _paletteBuffer.Upload(commandList, batch.Palettes().data(), matrixCount);

    for (const SkinDispatch &dispatch : batch.Dispatches())
    {
        const uint32_t groups = (dispatch.vertexCount + kSkinWorkgroupSize - 1u) / kSkinWorkgroupSize;
        _shader.Dispatch(commandList, _bindingSet, groups, 1u, 1u, &dispatch, sizeof(dispatch));
    }
}

} // namespace Assisi::Render
