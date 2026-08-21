/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshCuller.hpp>

#include <algorithm>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/Material.hpp>
#include <Assisi/Render/MeshBuffer.hpp>

namespace Assisi::Render
{

namespace
{
// mesh_cull.comp's push_constant block. 6 frustum planes + one uvec4 of counts.
// std430: vec4 arrays and uvec4 are 16-byte aligned, so no manual padding.
struct CullPushConstants
{
    glm::vec4 planes[6];
    glm::uvec4 counts; // x = object count, y = cull enabled (0/1), z = masked-half batch offset, w unused
};
static_assert(sizeof(CullPushConstants) == 112, "CullPushConstants must match mesh_cull.comp's push_constant block.");

// The DrawArgs the cull shader writes == VkDrawIndexedIndirectCommand (5 packed
// 32-bit fields), so the output indirect buffer's element stride is 20.
static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 20,
              "DrawIndexedIndirectArguments must match VkDrawIndexedIndirectCommand's packed layout.");
constexpr uint32_t kIndirectStride = 20u;

// Output instance-record stride — must match Render's InstanceData / mesh.vert
// (mat4 + uint, 80-byte std430 array stride). MeshPass.cpp static_asserts the C++ side.
constexpr uint32_t kInstanceStride = 80u;

// Generous initial capacities so typical scenes never grow a buffer mid-frame
// (a growth swaps a handle and forces a binding-set rebuild). Grown geometrically.
constexpr uint32_t kInitialObjects   = 4096u;
constexpr uint32_t kInitialMeshes    = 1024u;
constexpr uint32_t kInitialSubmeshes = 8192u;
constexpr uint32_t kInitialMaterials = 8192u;
constexpr uint32_t kInitialDraws     = 8192u;
} // namespace

// ---- CullTables / CullTableBuilder (pure, unit-testable) --------------------

void CullTables::Clear()
{
    objects.clear();
    meshDescs.clear();
    submeshes.clear();
    objectMaterials.clear();
    batchTemplates.clear();
    drawCapacity = 0;
    hasMaskedMaterials = false;
}

void CullTableBuilder::Reset()
{
    _tables.Clear();
    _meshIndex.clear();
    _batchReserve.clear();
}

void CullTableBuilder::AddInstanceRaw(const void *meshKey, const MeshGeometry &geometry, const glm::mat4 &model,
                                      std::span<const uint32_t> materialIds)
{
    const uint32_t lod0Count = static_cast<uint32_t>(geometry.lod0Submeshes.size());
    if (lod0Count == 0)
    {
        return; // no LOD0 geometry — nothing to draw
    }

    // Intern the mesh's descriptor on first sight; later instances reuse the index.
    uint32_t meshDescIndex;
    if (const auto it = _meshIndex.find(meshKey); it != _meshIndex.end())
    {
        meshDescIndex = it->second;
    }
    else
    {
        GpuMeshDesc desc;
        desc.sphere       = geometry.sphere;
        desc.aabbMin      = geometry.aabbMin;
        desc.aabbMax      = geometry.aabbMax;
        desc.vertexBase   = geometry.vertexBase;
        desc.indexBase    = geometry.indexBase;
        desc.firstSubmesh = static_cast<uint32_t>(_tables.submeshes.size());
        desc.submeshCount = lod0Count;
        _tables.submeshes.insert(_tables.submeshes.end(), geometry.lod0Submeshes.begin(),
                                 geometry.lod0Submeshes.end());

        meshDescIndex = static_cast<uint32_t>(_tables.meshDescs.size());
        _tables.meshDescs.push_back(desc);
        _meshIndex.emplace(meshKey, meshDescIndex);
    }

    GpuObject obj;
    obj.model         = model;
    obj.meshDescIndex = meshDescIndex;
    obj.materialBase  = static_cast<uint32_t>(_tables.objectMaterials.size());
    // Mirror the CPU path exactly: a submesh's material is materialIds[slot],
    // skipped when the slot is out of range or unresolved (kNoMaterial). The slice
    // length is the resolved-material count.
    obj.materialCount = static_cast<uint32_t>(materialIds.size());
    _tables.objectMaterials.insert(_tables.objectMaterials.end(), materialIds.begin(), materialIds.end());
    for (const uint32_t packed : materialIds)
    {
        // The sentinel has every bit set, so it must be excluded before the flag is
        // read off the top one — otherwise an unresolved slot would look masked and
        // grow the command table for a submesh that draws nothing.
        if (packed != kNoMaterial && (packed & kCullMaterialMaskedBit) != 0u)
        {
            _tables.hasMaskedMaterials = true;
        }
    }

    _tables.objects.push_back(obj);
    _tables.drawCapacity += lod0Count;
}

void CullTableBuilder::Finalize()
{
    // One draw-command template per (batch, live pipeline), batches being the
    // submeshes[] entries. Each gets a contiguous instance region, laid out end to
    // end — so the whole instance buffer is partitioned into regions the cull pass
    // packs survivors into. Opaque commands lead and masked ones follow, each half
    // in batch order, so a pipeline's commands draw in one multi-draw.
    const uint32_t batchCount = _tables.BatchCount();
    const uint32_t pipelineCount = _tables.hasMaskedMaterials ? kMeshPipelineCount : 1u;
    _tables.batchTemplates.assign(static_cast<size_t>(batchCount) * pipelineCount, GpuDrawArgs{});

    // Count what each region will actually hold. Reserving one slot per object in
    // both halves instead would double the instance buffer for any scene with a
    // single cutout material in it, most of it never written.
    _batchReserve.assign(_tables.batchTemplates.size(), 0u);
    for (const GpuObject &obj : _tables.objects)
    {
        const GpuMeshDesc &desc = _tables.meshDescs[obj.meshDescIndex];
        for (uint32_t s = 0; s < desc.submeshCount; ++s)
        {
            const uint32_t g = desc.firstSubmesh + s;
            const uint32_t slot = _tables.submeshes[g].materialSlot;
            if (slot >= obj.materialCount)
            {
                continue; // out of range — the cull shader skips it, as the CPU path does
            }
            const uint32_t packed = _tables.objectMaterials[obj.materialBase + slot];
            if (packed == kNoMaterial)
            {
                continue; // unresolved — drawn by neither pipeline
            }
            const uint32_t pipeline = (packed & kCullMaterialMaskedBit) != 0u ? 1u : 0u;
            ++_batchReserve[pipeline * batchCount + g];
        }
    }

    uint32_t instanceOffset = 0;
    for (uint32_t p = 0; p < pipelineCount; ++p)
    {
        for (const GpuMeshDesc &desc : _tables.meshDescs)
        {
            for (uint32_t s = 0; s < desc.submeshCount; ++s)
            {
                const uint32_t g = desc.firstSubmesh + s;
                const uint32_t command = p * batchCount + g;
                const GpuSubMesh &sm = _tables.submeshes[g];
                GpuDrawArgs &t  = _tables.batchTemplates[command];
                t.indexCount    = sm.indexCount;
                t.instanceCount = 0u; // grown atomically by the cull pass
                t.firstIndex    = desc.indexBase + sm.indexOffset;
                t.vertexOffset  = static_cast<int32_t>(desc.vertexBase);
                t.firstInstance = instanceOffset; // this command's reserved instance base
                instanceOffset += _batchReserve[command];
            }
        }
    }
}

void CullTableBuilder::AddInstance(const MeshBuffer *mesh, const glm::mat4 &model,
                                   std::span<const Material *const> slotMaterials)
{
    if (mesh == nullptr)
    {
        return;
    }
    const std::vector<Geometry::SubMesh> &subMeshes = mesh->SubMeshes();
    const std::vector<Geometry::LodRange> &lods      = mesh->Lods();
    const Geometry::LodRange lod0 =
        !lods.empty() ? lods.front() : Geometry::LodRange{0, static_cast<uint32_t>(subMeshes.size())};
    if (lod0.SubMeshCount == 0)
    {
        return; // no LOD0 geometry — nothing to draw
    }

    // Extract the mesh's LOD0 geometry + its resolved material ids into scratch,
    // then pack through the pure core (kNoMaterial for an out-of-range/null slot,
    // matching the CPU path's `material == nullptr` skip).
    _submeshScratch.clear();
    for (uint32_t i = 0; i < lod0.SubMeshCount; ++i)
    {
        const Geometry::SubMesh &sm = subMeshes[lod0.FirstSubMesh + i];
        _submeshScratch.push_back(GpuSubMesh{sm.IndexOffset, sm.IndexCount, sm.MaterialSlot, 0u});
    }
    _materialScratch.clear();
    for (const Material *mat : slotMaterials)
    {
        // Material::Pipeline is the one place the alpha mode decides where a
        // material draws; carrying its answer here is what keeps this path and the
        // CPU draw list from disagreeing about the same material.
        _materialScratch.push_back(mat == nullptr ? kNoMaterial
                                                  : mat->Id() | (mat->IsAlphaMasked() ? kCullMaterialMaskedBit : 0u));
    }

    MeshGeometry geometry;
    const Geometry::BoundingSphere &sphere = mesh->LocalBounds();
    geometry.sphere        = glm::vec4(sphere.center, sphere.radius);
    const Geometry::Aabb &aabb = mesh->LocalAabb();
    geometry.aabbMin       = glm::vec4(aabb.min, 0.f);
    geometry.aabbMax       = glm::vec4(aabb.max, 0.f);
    geometry.vertexBase    = mesh->VertexBase();
    geometry.indexBase     = mesh->IndexBase();
    geometry.lod0Submeshes = _submeshScratch;

    AddInstanceRaw(mesh, geometry, model, _materialScratch);
}

// ---- MeshCuller (device) ---------------------------------------------------

bool MeshCuller::Initialize(nvrhi::IDevice *device)
{
    _device = device;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)); // objects
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1)); // meshDescs
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2)); // submeshes
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3)); // objectMaterials
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)); // outInstances
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1)); // outDraws
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2)); // stats
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(CullPushConstants)));
    if (!_cullShader.Initialize(device, "shaders/mesh_cull.comp.spv", layoutDesc))
    {
        Core::Log::Error("MeshCuller: failed to build the mesh_cull compute pipeline.");
        return false;
    }

    EnsureInput(_objectBuffer, sizeof(GpuObject), kInitialObjects, "MeshCuller::Objects");
    EnsureInput(_meshDescBuffer, sizeof(GpuMeshDesc), kInitialMeshes, "MeshCuller::MeshDescs");
    EnsureInput(_submeshBuffer, sizeof(GpuSubMesh), kInitialSubmeshes, "MeshCuller::SubMeshes");
    EnsureInput(_objectMaterialBuffer, sizeof(uint32_t), kInitialMaterials, "MeshCuller::ObjectMaterials");
    EnsureInstanceCapacity(kInitialDraws);
    EnsureIndirectCapacity(kInitialDraws);
    RebuildBindingSet();

    if (_cullBindingSet == nullptr)
    {
        Core::Log::Error("MeshCuller: failed to create the cull binding set.");
        return false;
    }
    return true;
}

void MeshCuller::EnsureInput(Buffer &buffer, uint32_t stride, uint32_t neededElements, const char *debugName)
{
    if (buffer.IsValid() && neededElements <= buffer.CapacityElements())
    {
        return;
    }
    const uint32_t capacity = std::max(buffer.CapacityElements() * 2u, neededElements);
    buffer.Create(_device, stride, capacity, /*allowUnorderedAccess=*/ false, debugName);
    _bindingSetDirty = true;
}

void MeshCuller::EnsureInstanceCapacity(uint32_t neededElements)
{
    if (_instanceBuffer.IsValid() && neededElements <= _instanceBuffer.CapacityElements())
    {
        return;
    }
    const uint32_t capacity = std::max(_instanceBuffer.CapacityElements() * 2u, neededElements);
    _instanceBuffer.Create(_device, kInstanceStride, capacity, /*allowUnorderedAccess=*/ true, "MeshCuller::Instances");
    _bindingSetDirty = true;
}

void MeshCuller::EnsureIndirectCapacity(uint32_t neededCommands)
{
    if (_statsBuffer == nullptr)
    {
        // Two-uint stats {survivor instances, live batches}, cleared each frame and
        // grown by the shader (UAV), copied to the readback ring for the overlay.
        // keepInitialState seeds the tracked state to UnorderedAccess so NVRHI
        // tracks the buffer and barriers it (UAV↔CopySource for the readback copy).
        nvrhi::BufferDesc statsDesc;
        statsDesc.byteSize          = 2u * sizeof(uint32_t);
        statsDesc.structStride      = sizeof(uint32_t);
        statsDesc.canHaveUAVs       = true;
        statsDesc.initialState      = nvrhi::ResourceStates::UnorderedAccess;
        statsDesc.keepInitialState  = true;
        statsDesc.debugName         = "MeshCuller::Stats";
        _statsBuffer                = _device->createBuffer(statsDesc);
        _bindingSetDirty            = true;

        // CPU-readable ring for the stats readback (overlay only). Fixed 8-byte
        // buffers, never grow; cpuAccess=Read makes them host-visible copy targets
        // that don't participate in state tracking.
        for (nvrhi::BufferHandle &readback : _statsReadback)
        {
            nvrhi::BufferDesc readbackDesc;
            readbackDesc.byteSize  = 2u * sizeof(uint32_t);
            readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
            readbackDesc.debugName = "MeshCuller::StatsReadback";
            readback               = _device->createBuffer(readbackDesc);
        }
    }

    if (_indirectBuffer != nullptr && neededCommands <= _indirectCapacity)
    {
        return;
    }
    const uint32_t capacity = std::max(std::max(_indirectCapacity * 2u, neededCommands), kInitialDraws);

    // One command per batch: uploaded with the CPU templates each frame (which
    // resets instanceCount to 0), grown atomically by the cull pass, then read as
    // indirect args. keepInitialState-seeded UnorderedAccess so NVRHI tracks and
    // barriers it CopyDest (template upload) → UnorderedAccess (pass) → IndirectArgument (draw).
    nvrhi::BufferDesc desc;
    desc.byteSize           = static_cast<uint64_t>(kIndirectStride) * capacity;
    desc.structStride       = kIndirectStride;
    desc.canHaveUAVs        = true;
    desc.isDrawIndirectArgs = true;
    desc.initialState       = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState   = true;
    desc.debugName          = "MeshCuller::Indirect";
    _indirectBuffer         = _device->createBuffer(desc);
    _indirectCapacity       = capacity;
    _bindingSetDirty        = true;
}

void MeshCuller::RebuildBindingSet()
{
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, _objectBuffer.NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, _meshDescBuffer.NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, _submeshBuffer.NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, _objectMaterialBuffer.NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, _instanceBuffer.NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, _indirectBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, _statsBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CullPushConstants)));
    _cullBindingSet  = _device->createBindingSet(setDesc, _cullShader.BindingLayout());
    _bindingSetDirty = false;
}

void MeshCuller::Cull(nvrhi::ICommandList *commandList, const std::array<glm::vec4, 6> &frustumPlanes,
                      const CullTables &tables, bool frustumCull)
{
    const uint32_t commandCount = tables.OpaqueCommandCount() + tables.MaskedCommandCount();
    if (!IsValid() || tables.Empty() || tables.batchTemplates.size() != commandCount)
    {
        _lastMaxDraws       = 0;
        _lastOpaqueCommands = 0;
        _lastMaskedCommands = 0;
        return; // empty, or Finalize() wasn't called — nothing to draw
    }

    const uint32_t batchCount = tables.BatchCount();

    // A buffer that binds must have at least one element even when its table is
    // empty (an object with zero material slots leaves objectMaterials empty).
    EnsureInput(_objectBuffer, sizeof(GpuObject), static_cast<uint32_t>(tables.objects.size()), "MeshCuller::Objects");
    EnsureInput(_meshDescBuffer, sizeof(GpuMeshDesc), static_cast<uint32_t>(tables.meshDescs.size()),
                "MeshCuller::MeshDescs");
    EnsureInput(_submeshBuffer, sizeof(GpuSubMesh), std::max<uint32_t>(1u, batchCount), "MeshCuller::SubMeshes");
    EnsureInput(_objectMaterialBuffer, sizeof(uint32_t),
                std::max<uint32_t>(1u, static_cast<uint32_t>(tables.objectMaterials.size())),
                "MeshCuller::ObjectMaterials");
    EnsureInstanceCapacity(std::max<uint32_t>(1u, tables.drawCapacity));
    EnsureIndirectCapacity(std::max<uint32_t>(1u, commandCount));
    if (_bindingSetDirty)
    {
        RebuildBindingSet();
    }

    _objectBuffer.Upload(commandList, tables.objects.data(), static_cast<uint32_t>(tables.objects.size()));
    _meshDescBuffer.Upload(commandList, tables.meshDescs.data(), static_cast<uint32_t>(tables.meshDescs.size()));
    _submeshBuffer.Upload(commandList, tables.submeshes.data(), batchCount);
    if (!tables.objectMaterials.empty())
    {
        _objectMaterialBuffer.Upload(commandList, tables.objectMaterials.data(),
                                     static_cast<uint32_t>(tables.objectMaterials.size()));
    }

    // Upload the per-batch draw-command templates. This both sets each batch's
    // geometry range + reserved instance base AND resets instanceCount to 0 (the
    // cull pass grows it), so no separate clear of the indirect buffer is needed.
    commandList->writeBuffer(_indirectBuffer, tables.batchTemplates.data(),
                             static_cast<size_t>(commandCount) * kIndirectStride);

    // Read back the stats {instances, batches} written kReadbackFrames ago (the
    // slot about to be overwritten — safely retired), before this frame's copy.
    if (_readbackPrimed >= kReadbackFrames)
    {
        if (void *mapped = _device->mapBuffer(_statsReadback[_readbackCursor], nvrhi::CpuAccessMode::Read))
        {
            const uint32_t *stats  = static_cast<const uint32_t *>(mapped);
            _lastSurvivorInstances = stats[0];
            _lastSurvivorBatches   = stats[1];
            _device->unmapBuffer(_statsReadback[_readbackCursor]);
        }
    }

    // Reset the stats counters before the pass grows them.
    commandList->clearBufferUInt(_statsBuffer, 0u);

    _lastMaxDraws       = tables.drawCapacity;
    _lastOpaqueCommands = tables.OpaqueCommandCount();
    _lastMaskedCommands = tables.MaskedCommandCount();

    CullPushConstants pc;
    std::copy(frustumPlanes.begin(), frustumPlanes.end(), pc.planes);
    // counts.z is what a masked instance adds to its batch index to reach the
    // masked half — and it is zero when there is no masked half, which is the whole
    // of "a frame with no cutout material behaves as it always did".
    pc.counts = glm::uvec4(static_cast<uint32_t>(tables.objects.size()), frustumCull ? 1u : 0u,
                           tables.MaskedCommandCount() != 0u ? batchCount : 0u, 0u);

    const uint32_t groups = (static_cast<uint32_t>(tables.objects.size()) + 63u) / 64u;
    _cullShader.Dispatch(commandList, _cullBindingSet, groups, 1u, 1u, &pc, sizeof(pc));

    // Snapshot this frame's stats into the ring for a later frame to read back.
    commandList->copyBuffer(_statsReadback[_readbackCursor], 0, _statsBuffer, 0, 2u * sizeof(uint32_t));
    _readbackCursor = (_readbackCursor + 1u) % kReadbackFrames;
    if (_readbackPrimed < kReadbackFrames)
    {
        ++_readbackPrimed;
    }
}

uint32_t MeshCuller::SurvivorInstanceCount() const
{
    // Until the ring is primed the readback slots hold garbage, so report the
    // candidate total (assume everything survives) rather than flash a bogus count.
    return _readbackPrimed >= kReadbackFrames ? _lastSurvivorInstances : _lastMaxDraws;
}

uint32_t MeshCuller::SurvivorBatchCount() const
{
    // Before priming, the live-batch count is unknown; report the total command
    // count (all potentially non-empty) as the stand-in.
    return _readbackPrimed >= kReadbackFrames ? _lastSurvivorBatches
                                              : _lastOpaqueCommands + _lastMaskedCommands;
}

} // namespace Assisi::Render
