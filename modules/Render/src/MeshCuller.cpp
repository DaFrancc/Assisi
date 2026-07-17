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
    glm::vec4  planes[6];
    glm::uvec4 counts; // x = object count, y = max draw capacity, z = cull enabled, w unused
};
static_assert(sizeof(CullPushConstants) == 112, "CullPushConstants must match mesh_cull.comp's push_constant block.");

// The DrawArgs the cull shader writes == VkDrawIndexedIndirectCommand (5 packed
// 32-bit fields), so the output indirect buffer's element stride is 20.
static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 20,
              "DrawIndexedIndirectArguments must match VkDrawIndexedIndirectCommand's packed layout.");
constexpr uint32_t kIndirectStride = 20u;

// Output instance-record stride — must match Render's InstanceData / cube_min.vert
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
    drawCapacity = 0;
}

void CullTableBuilder::Reset()
{
    _tables.Clear();
    _meshIndex.clear();
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

    _tables.objects.push_back(obj);
    _tables.drawCapacity += lod0Count;
}

void CullTableBuilder::AddInstance(const MeshBuffer *mesh, const glm::mat4 &model,
                                   std::span<const Material *const> slotMaterials)
{
    if (mesh == nullptr)
    {
        return;
    }
    const std::vector<Geometry::SubMesh>  &subMeshes = mesh->SubMeshes();
    const std::vector<Geometry::LodRange> &lods      = mesh->Lods();
    const Geometry::LodRange               lod0 =
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
        _materialScratch.push_back(mat != nullptr ? mat->Id() : kNoMaterial);
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
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2)); // drawCount
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
    buffer.Create(_device, stride, capacity, /*allowUnorderedAccess=*/false, debugName);
    _bindingSetDirty = true;
}

void MeshCuller::EnsureInstanceCapacity(uint32_t neededElements)
{
    if (_instanceBuffer.IsValid() && neededElements <= _instanceBuffer.CapacityElements())
    {
        return;
    }
    const uint32_t capacity = std::max(_instanceBuffer.CapacityElements() * 2u, neededElements);
    _instanceBuffer.Create(_device, kInstanceStride, capacity, /*allowUnorderedAccess=*/true, "MeshCuller::Instances");
    _bindingSetDirty = true;
}

void MeshCuller::EnsureIndirectCapacity(uint32_t neededDraws)
{
    if (_countBuffer == nullptr)
    {
        // Single-uint counter, written by the shader (UAV) and read by
        // drawIndexedIndirectCount (indirect). keepInitialState seeds the tracked
        // state to UnorderedAccess (the compute-write state) — NVRHI then tracks
        // the buffer and inserts the UAV→IndirectArgument barrier for the draw
        // (and back to UAV for next frame's clear). Not permanent state: NVRHI's
        // keepInitialState seeds the tracker, it doesn't pin the buffer.
        nvrhi::BufferDesc countDesc;
        countDesc.byteSize           = sizeof(uint32_t);
        countDesc.structStride       = sizeof(uint32_t);
        countDesc.canHaveUAVs        = true;
        countDesc.isDrawIndirectArgs = true;
        countDesc.initialState       = nvrhi::ResourceStates::UnorderedAccess;
        countDesc.keepInitialState   = true;
        countDesc.debugName          = "MeshCuller::DrawCount";
        _countBuffer                 = _device->createBuffer(countDesc);
        _bindingSetDirty             = true;

        // CPU-readable ring for the survivor-count readback (overlay only). Fixed
        // 4-byte buffers, never grow; cpuAccess=Read makes them host-visible copy
        // targets that don't participate in state tracking.
        for (nvrhi::BufferHandle &readback : _countReadback)
        {
            nvrhi::BufferDesc readbackDesc;
            readbackDesc.byteSize  = sizeof(uint32_t);
            readbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
            readbackDesc.debugName = "MeshCuller::DrawCountReadback";
            readback               = _device->createBuffer(readbackDesc);
        }
    }

    if (_indirectBuffer != nullptr && neededDraws <= _indirectCapacity)
    {
        return;
    }
    const uint32_t capacity = std::max(std::max(_indirectCapacity * 2u, neededDraws), kInitialDraws);

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
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, _countBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CullPushConstants)));
    _cullBindingSet  = _device->createBindingSet(setDesc, _cullShader.BindingLayout());
    _bindingSetDirty = false;
}

void MeshCuller::Cull(nvrhi::ICommandList *commandList, const std::array<glm::vec4, 6> &frustumPlanes,
                      const CullTables &tables, bool frustumCull)
{
    if (!IsValid() || tables.Empty())
    {
        _lastMaxDraws = 0;
        return;
    }

    // A buffer that binds must have at least one element even when its table is
    // empty (an object with zero material slots leaves objectMaterials empty).
    EnsureInput(_objectBuffer, sizeof(GpuObject), static_cast<uint32_t>(tables.objects.size()), "MeshCuller::Objects");
    EnsureInput(_meshDescBuffer, sizeof(GpuMeshDesc), static_cast<uint32_t>(tables.meshDescs.size()),
                "MeshCuller::MeshDescs");
    EnsureInput(_submeshBuffer, sizeof(GpuSubMesh),
                std::max<uint32_t>(1u, static_cast<uint32_t>(tables.submeshes.size())), "MeshCuller::SubMeshes");
    EnsureInput(_objectMaterialBuffer, sizeof(uint32_t),
                std::max<uint32_t>(1u, static_cast<uint32_t>(tables.objectMaterials.size())),
                "MeshCuller::ObjectMaterials");
    EnsureInstanceCapacity(std::max<uint32_t>(1u, tables.drawCapacity));
    EnsureIndirectCapacity(std::max<uint32_t>(1u, tables.drawCapacity));
    if (_bindingSetDirty)
    {
        RebuildBindingSet();
    }

    _objectBuffer.Upload(commandList, tables.objects.data(), static_cast<uint32_t>(tables.objects.size()));
    _meshDescBuffer.Upload(commandList, tables.meshDescs.data(), static_cast<uint32_t>(tables.meshDescs.size()));
    if (!tables.submeshes.empty())
    {
        _submeshBuffer.Upload(commandList, tables.submeshes.data(), static_cast<uint32_t>(tables.submeshes.size()));
    }
    if (!tables.objectMaterials.empty())
    {
        _objectMaterialBuffer.Upload(commandList, tables.objectMaterials.data(),
                                     static_cast<uint32_t>(tables.objectMaterials.size()));
    }

    // Read back the survivor count written kReadbackFrames ago (the slot about to
    // be overwritten — safely retired), before this frame's copy clobbers it.
    if (_readbackPrimed >= kReadbackFrames)
    {
        if (void *mapped = _device->mapBuffer(_countReadback[_readbackCursor], nvrhi::CpuAccessMode::Read))
        {
            _lastSurvivorCount = *static_cast<const uint32_t *>(mapped);
            _device->unmapBuffer(_countReadback[_readbackCursor]);
        }
    }

    // Reset the atomic draw counter before the pass appends to it.
    commandList->clearBufferUInt(_countBuffer, 0u);

    _lastMaxDraws = tables.drawCapacity;

    CullPushConstants pc;
    std::copy(frustumPlanes.begin(), frustumPlanes.end(), pc.planes);
    pc.counts = glm::uvec4(static_cast<uint32_t>(tables.objects.size()), tables.drawCapacity, frustumCull ? 1u : 0u, 0u);

    const uint32_t groups = (static_cast<uint32_t>(tables.objects.size()) + 63u) / 64u;
    _cullShader.Dispatch(commandList, _cullBindingSet, groups, 1u, 1u, &pc, sizeof(pc));

    // Snapshot this frame's count into the ring for a later frame to read back.
    commandList->copyBuffer(_countReadback[_readbackCursor], 0, _countBuffer, 0, sizeof(uint32_t));
    _readbackCursor = (_readbackCursor + 1u) % kReadbackFrames;
    if (_readbackPrimed < kReadbackFrames)
    {
        ++_readbackPrimed;
    }
}

uint32_t MeshCuller::SurvivorDrawCount() const
{
    // Until the ring is primed the readback slots hold garbage, so report the
    // capacity (assume everything survives) rather than flash a bogus count.
    return _readbackPrimed >= kReadbackFrames ? _lastSurvivorCount : _lastMaxDraws;
}

} // namespace Assisi::Render
