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
// mesh_cull.comp's push_constant block. std430: uvec4 is 16-byte aligned, so no
// manual padding.
struct CullPushConstants
{
    glm::uvec4 counts; // x = object count, y = cull enabled (0/1), zw unused
    /// Where each MeshPipeline's command block starts, indexed by the pipeline
    /// packed into a material entry. One lane per pipeline, which is why the enum
    /// is capped at four.
    glm::uvec4 pipelineBase;
};
static_assert(kMeshPipelineCount == 4, "pipelineBase is a uvec4, one lane per pipeline.");
static_assert(sizeof(CullPushConstants) == 32, "CullPushConstants must match mesh_cull.comp's push_constant block.");

// mesh_cull.comp's CullViewConstants uniform block. std140: vec4 arrays have a
// 16-byte stride and every member here is a vec4, so the layouts agree.
struct CullViewConstants
{
    glm::vec4 planes[6];
    glm::vec4 eye; // xyz = camera position, w = tan(fovY / 2)
    glm::vec4 lod; // x = bias, yzw unused
};
static_assert(sizeof(CullViewConstants) == 128, "CullViewConstants must match mesh_cull.comp's uniform block.");

// The stats buffer: survivor instances, live batches, culled objects, then
// survivors per LOD bucket. Must match mesh_cull.comp's Stats block.
constexpr uint32_t kStatsLodWord = 3u;
constexpr uint32_t kStatsWords   = kStatsLodWord + kCullLodBuckets;

// The DrawArgs the cull shader writes == VkDrawIndexedIndirectCommand (5 packed
// 32-bit fields), so the output indirect buffer's element stride is 20.
static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 20,
              "DrawIndexedIndirectArguments must match VkDrawIndexedIndirectCommand's packed layout.");
constexpr uint32_t kIndirectStride = 20u;

// Output instance-record stride — must match Render's InstanceData / mesh.vert
// (mat4 + uint, 80-byte std430 array stride). MeshPass.cpp static_asserts the C++ side.
constexpr uint32_t kInstanceStride = 80u;

// Threads per cull workgroup — one per scene object. Must equal mesh_cull.comp's
// local_size_x: too small and the tail of the object table is never dispatched,
// too large and the surplus threads return on the bounds check.
constexpr uint32_t kCullWorkgroupSize = 64u;

// Generous initial capacities so typical scenes never grow a buffer mid-frame
// (a growth swaps a handle and forces a binding-set rebuild). Grown geometrically.
constexpr uint32_t kInitialObjects   = 4096u;
constexpr uint32_t kInitialMeshes    = 1024u;
constexpr uint32_t kInitialLods      = 4096u;
constexpr uint32_t kInitialSubmeshes = 8192u;
constexpr uint32_t kInitialMaterials = 8192u;
constexpr uint32_t kInitialDraws     = 8192u;
} // namespace

// ---- CullTables / CullTableBuilder (pure, unit-testable) --------------------

void CullTables::Clear()
{
    objects.clear();
    meshDescs.clear();
    lods.clear();
    submeshes.clear();
    objectMaterials.clear();
    batchTemplates.clear();
    drawCapacity = 0;
    pipelineMask = 0;
}

void CullTableBuilder::Reset()
{
    _tables.Clear();
    _meshIndex.clear();
    _batchReserve.clear();
}

void CullTableBuilder::AddInstanceRaw(const void *meshKey, const MeshGeometry &geometry, const glm::mat4 &model,
                                      std::span<const uint32_t> materialIds, uint32_t level)
{
    if (geometry.submeshes.empty())
    {
        return; // no geometry — nothing to draw
    }

    // Intern the mesh's descriptor on first sight; later instances of it reuse
    // the index.
    uint32_t meshDescIndex;
    if (const auto it = _meshIndex.find(meshKey); it != _meshIndex.end())
    {
        meshDescIndex = it->second;
    }
    else
    {
        GpuMeshDesc desc;
        desc.sphere     = geometry.sphere;
        desc.aabbMin    = geometry.aabbMin;
        desc.aabbMax    = geometry.aabbMax;
        desc.vertexBase = geometry.vertexBase;
        desc.indexBase  = geometry.indexBase;
        desc.firstLod   = static_cast<uint32_t>(_tables.lods.size());

        // Each level's run is copied out on its own, so a level is always one
        // contiguous run of batches whatever order or overlap the source ranges
        // have.
        const Geometry::LodRange whole{0, static_cast<uint32_t>(geometry.submeshes.size())};
        const std::span<const Geometry::LodRange> chain =
            geometry.lods.empty() ? std::span<const Geometry::LodRange>(&whole, 1u) : geometry.lods;
        for (uint32_t chainLevel = 0; chainLevel < chain.size(); ++chainLevel)
        {
            const Geometry::LodRange &range = chain[chainLevel];
            const uint32_t first = std::min<uint32_t>(range.FirstSubMesh, static_cast<uint32_t>(geometry.submeshes.size()));
            const uint32_t count = std::min<uint32_t>(range.SubMeshCount,
                                                      static_cast<uint32_t>(geometry.submeshes.size()) - first);
            _tables.lods.push_back(GpuLod{.firstSubmesh = static_cast<uint32_t>(_tables.submeshes.size()),
                                          .submeshCount = count,
                                          .screenSizeThreshold = Geometry::LodScreenSizeThreshold(range, chainLevel)});
            _tables.submeshes.insert(_tables.submeshes.end(), geometry.submeshes.begin() + first,
                                     geometry.submeshes.begin() + first + count);
        }
        desc.lodCount = static_cast<uint32_t>(chain.size());

        meshDescIndex = static_cast<uint32_t>(_tables.meshDescs.size());
        _tables.meshDescs.push_back(desc);
        _meshIndex.emplace(meshKey, meshDescIndex);
    }

    const GpuMeshDesc &desc = _tables.meshDescs[meshDescIndex];
    GpuObject obj;
    obj.model         = model;
    obj.meshDescIndex = meshDescIndex;
    // A single level is named rather than measured, so a mesh without a chain
    // costs the shader no measurement and reserves exactly one level.
    obj.lodLevel      = level == kMeasureLod && desc.lodCount > 1 ? kMeasureLod
                                                                  : std::min(level, desc.lodCount - 1u);
    obj.materialBase  = static_cast<uint32_t>(_tables.objectMaterials.size());
    // Mirror the CPU path exactly: a submesh's material is materialIds[slot],
    // skipped when the slot is out of range or unresolved (kNoMaterial). The slice
    // length is the resolved-material count.
    obj.materialCount = static_cast<uint32_t>(materialIds.size());
    _tables.objectMaterials.insert(_tables.objectMaterials.end(), materialIds.begin(), materialIds.end());
    for (const uint32_t packed : materialIds)
    {
        // The sentinel has every bit set, so it must be excluded before the
        // pipeline is read off the top ones — otherwise an unresolved slot would
        // light a pipeline and grow the command table for a submesh that draws
        // nothing.
        if (packed != kNoMaterial)
        {
            _tables.pipelineMask |= 1u << static_cast<uint32_t>(CullMaterialPipeline(packed));
        }
    }

    _tables.objects.push_back(obj);
    const auto [firstLod, lodCount] = ReachableLods(obj);
    for (uint32_t lod = firstLod; lod < firstLod + lodCount; ++lod)
    {
        _tables.drawCapacity += _tables.lods[lod].submeshCount;
    }
}

std::pair<uint32_t, uint32_t> CullTableBuilder::ReachableLods(const GpuObject &obj) const
{
    const GpuMeshDesc &desc = _tables.meshDescs[obj.meshDescIndex];
    return obj.lodLevel == kMeasureLod ? std::pair{desc.firstLod, desc.lodCount}
                                       : std::pair{desc.firstLod + obj.lodLevel, 1u};
}

void CullTableBuilder::Finalize()
{
    // One draw-command template per (batch, live pipeline), batches being the
    // submeshes[] entries. Each gets a contiguous instance region, laid out end to
    // end — so the whole instance buffer is partitioned into regions the cull pass
    // packs survivors into. A pipeline's commands form one contiguous block, in
    // pipeline order, so a block draws in one multi-draw; a pipeline nothing uses
    // has no block at all.
    _tables.batchTemplates.assign(_tables.TotalCommandCount(), GpuDrawArgs{});

    // Count what each region will actually hold. Reserving one slot per object in
    // every block instead would multiply the instance buffer by the number of live
    // pipelines, most of it never written. A measured object counts toward every
    // level it could land on, and may write any one of them.
    _batchReserve.assign(_tables.batchTemplates.size(), 0u);
    for (const GpuObject &obj : _tables.objects)
    {
        const auto [firstLod, lodCount] = ReachableLods(obj);
        for (uint32_t lod = firstLod; lod < firstLod + lodCount; ++lod)
        {
            const GpuLod &range = _tables.lods[lod];
            for (uint32_t s = 0; s < range.submeshCount; ++s)
            {
                const uint32_t g = range.firstSubmesh + s;
                const uint32_t slot = _tables.submeshes[g].materialSlot;
                if (slot >= obj.materialCount)
                {
                    continue; // out of range — the cull shader skips it, as the CPU path does
                }
                const uint32_t packed = _tables.objectMaterials[obj.materialBase + slot];
                if (packed == kNoMaterial)
                {
                    continue; // unresolved — drawn by no pipeline
                }
                ++_batchReserve[_tables.CommandBase(CullMaterialPipeline(packed)) + g];
            }
        }
    }

    uint32_t instanceOffset = 0;
    for (uint32_t p = 0; p < kMeshPipelineCount; ++p)
    {
        const MeshPipeline pipeline = static_cast<MeshPipeline>(p);
        if (!_tables.UsesPipeline(pipeline))
        {
            continue;
        }
        const uint32_t base = _tables.CommandBase(pipeline);
        for (const GpuMeshDesc &desc : _tables.meshDescs)
        {
            for (uint32_t lod = desc.firstLod; lod < desc.firstLod + desc.lodCount; ++lod)
            {
                const GpuLod &range = _tables.lods[lod];
                for (uint32_t s = 0; s < range.submeshCount; ++s)
                {
                    const uint32_t g = range.firstSubmesh + s;
                    const uint32_t command = base + g;
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
}

void CullTableBuilder::AddInstance(const MeshBuffer *mesh, const glm::mat4 &model,
                                   std::span<const Material *const> slotMaterials, uint32_t level)
{
    if (mesh == nullptr)
    {
        return;
    }
    // Extract the mesh's submeshes + its resolved material ids into scratch, then
    // pack through the pure core (kNoMaterial for an out-of-range/null slot,
    // matching the CPU path's `material == nullptr` skip). A mesh already interned
    // this frame reads neither, but the core decides that, and both are a short
    // copy.
    _submeshScratch.clear();
    for (const Geometry::SubMesh &sm : mesh->SubMeshes())
    {
        _submeshScratch.push_back(GpuSubMesh{sm.IndexOffset, sm.IndexCount, sm.MaterialSlot, 0u});
    }
    _materialScratch.clear();
    for (const Material *mat : slotMaterials)
    {
        // Material::Pipeline is the one place a material's properties decide where
        // it draws; carrying its answer here is what keeps this path and the CPU
        // draw list from disagreeing about the same material.
        _materialScratch.push_back(mat == nullptr ? kNoMaterial
                                                  : EncodeCullMaterial(mat->Id(), mat->Pipeline()));
    }

    MeshGeometry geometry;
    const Geometry::BoundingSphere &sphere = mesh->LocalBounds();
    geometry.sphere        = glm::vec4(sphere.center, sphere.radius);
    const Geometry::Aabb &aabb = mesh->LocalAabb();
    geometry.aabbMin       = glm::vec4(aabb.min, 0.f);
    geometry.aabbMax       = glm::vec4(aabb.max, 0.f);
    geometry.vertexBase    = mesh->VertexBase();
    geometry.indexBase     = mesh->IndexBase();
    geometry.submeshes     = _submeshScratch;
    geometry.lods          = mesh->Lods();

    AddInstanceRaw(mesh, geometry, model, _materialScratch, level);
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
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4)); // lods
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));       // view
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)); // outInstances
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1)); // outDraws
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2)); // stats
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(CullPushConstants)));
    if (!_cullShader.Initialize(device, "shaders/mesh_cull.comp.spv", layoutDesc))
    {
        Core::Log::Error("MeshCuller: failed to build the mesh_cull compute pipeline.");
        return false;
    }

    nvrhi::BufferDesc viewDesc;
    viewDesc.byteSize         = sizeof(CullViewConstants);
    viewDesc.isConstantBuffer = true;
    viewDesc.debugName        = "MeshCuller::View";
    viewDesc.initialState     = nvrhi::ResourceStates::ConstantBuffer;
    viewDesc.keepInitialState = true;
    _viewConstants            = _device->createBuffer(viewDesc);
    if (_viewConstants == nullptr)
    {
        Core::Log::Error("MeshCuller: failed to create the view constant buffer.");
        return false;
    }

    EnsureInput(_objectBuffer, sizeof(GpuObject), kInitialObjects, "MeshCuller::Objects");
    EnsureInput(_meshDescBuffer, sizeof(GpuMeshDesc), kInitialMeshes, "MeshCuller::MeshDescs");
    EnsureInput(_lodBuffer, sizeof(GpuLod), kInitialLods, "MeshCuller::Lods");
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
        // The stats words, cleared each frame and grown by the shader (UAV), copied
        // to the readback ring for the overlay. keepInitialState seeds the tracked
        // state to UnorderedAccess so NVRHI tracks the buffer and barriers it
        // (UAV↔CopySource for the readback copy).
        nvrhi::BufferDesc statsDesc;
        statsDesc.byteSize          = kStatsWords * sizeof(uint32_t);
        statsDesc.structStride      = sizeof(uint32_t);
        statsDesc.canHaveUAVs       = true;
        statsDesc.initialState      = nvrhi::ResourceStates::UnorderedAccess;
        statsDesc.keepInitialState  = true;
        statsDesc.debugName         = "MeshCuller::Stats";
        _statsBuffer                = _device->createBuffer(statsDesc);
        _bindingSetDirty            = true;

        // CPU-readable ring for the stats readback (overlay only). Fixed-size
        // buffers, never grow; cpuAccess=Read makes them host-visible copy targets
        // that don't participate in state tracking.
        for (nvrhi::BufferHandle &readback : _statsReadback)
        {
            nvrhi::BufferDesc readbackDesc;
            readbackDesc.byteSize  = kStatsWords * sizeof(uint32_t);
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
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, _lodBuffer.NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _viewConstants));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, _instanceBuffer.NativeBuffer()));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, _indirectBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, _statsBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CullPushConstants)));
    _cullBindingSet  = _device->createBindingSet(setDesc, _cullShader.BindingLayout());
    _bindingSetDirty = false;
}

void MeshCuller::Cull(nvrhi::ICommandList *commandList, const CullView &view, const CullTables &tables,
                      bool frustumCull)
{
    const uint32_t commandCount = tables.TotalCommandCount();
    if (!IsValid() || tables.Empty() || tables.batchTemplates.size() != commandCount)
    {
        _lastMaxDraws = 0;
        _lastCommandCounts.fill(0u);
        return; // empty, or Finalize() wasn't called — nothing to draw
    }

    const uint32_t batchCount = tables.BatchCount();

    // A buffer that binds must have at least one element even when its table is
    // empty (an object with zero material slots leaves objectMaterials empty).
    EnsureInput(_objectBuffer, sizeof(GpuObject), static_cast<uint32_t>(tables.objects.size()), "MeshCuller::Objects");
    EnsureInput(_meshDescBuffer, sizeof(GpuMeshDesc), static_cast<uint32_t>(tables.meshDescs.size()),
                "MeshCuller::MeshDescs");
    EnsureInput(_lodBuffer, sizeof(GpuLod), static_cast<uint32_t>(tables.lods.size()), "MeshCuller::Lods");
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
    _lodBuffer.Upload(commandList, tables.lods.data(), static_cast<uint32_t>(tables.lods.size()));
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
            _lastCulledObjects     = stats[2];
            std::copy(stats + kStatsLodWord, stats + kStatsWords, _lastSurvivorLods.begin());
            _device->unmapBuffer(_statsReadback[_readbackCursor]);
        }
    }

    CullViewConstants viewConstants;
    std::copy(view.frustumPlanes.begin(), view.frustumPlanes.end(), viewConstants.planes);
    viewConstants.eye = glm::vec4(view.cameraPosition, view.tanHalfFovY);
    viewConstants.lod = glm::vec4(view.lodBias, 0.f, 0.f, 0.f);
    commandList->writeBuffer(_viewConstants, &viewConstants, sizeof(viewConstants));

    // Reset the stats counters before the pass grows them.
    commandList->clearBufferUInt(_statsBuffer, 0u);

    _lastMaxDraws = tables.drawCapacity;
    for (uint32_t p = 0; p < kMeshPipelineCount; ++p)
    {
        _lastCommandCounts[p] = tables.CommandCount(static_cast<MeshPipeline>(p));
    }

    CullPushConstants pc;
    pc.counts =glm::uvec4(static_cast<uint32_t>(tables.objects.size()), frustumCull ? 1u : 0u, 0u, 0u);
    // Each lane is where that pipeline's command block starts, which is what an
    // instance adds to its batch index to reach its own command. A scene using only
    // the opaque pipeline leaves every base at 0 and behaves exactly as it did
    // before any other pipeline existed.
    for (uint32_t p = 0; p < kMeshPipelineCount; ++p)
    {
        pc.pipelineBase[static_cast<glm::length_t>(p)] = tables.CommandBase(static_cast<MeshPipeline>(p));
    }

    const uint32_t objectCount = static_cast<uint32_t>(tables.objects.size());
    const uint32_t groups = (objectCount + kCullWorkgroupSize - 1u) / kCullWorkgroupSize;
    _cullShader.Dispatch(commandList, _cullBindingSet, groups, 1u, 1u, &pc, sizeof(pc));

    // Snapshot this frame's stats into the ring for a later frame to read back.
    commandList->copyBuffer(_statsReadback[_readbackCursor], 0, _statsBuffer, 0, kStatsWords * sizeof(uint32_t));
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
    if (_readbackPrimed >= kReadbackFrames)
    {
        return _lastSurvivorBatches;
    }
    uint32_t total = 0;
    for (const uint32_t count : _lastCommandCounts)
    {
        total += count;
    }
    return total;
}

} // namespace Assisi::Render
