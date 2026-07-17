/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MeshCuller.hpp
/// @brief GPU-driven draw-list build (mesh-material stage F1): a compute pass
///        (mesh_cull.comp) frustum-culls every scene object and writes the
///        indirect draw commands + per-instance records itself, so the CPU
///        issues one drawIndexedIndirectCount instead of extracting/sorting a
///        draw list per frame.
///
/// Two halves, split so the CPU-side packing is unit-testable without a device:
///   - `CullTableBuilder` (pure): turns a set of (mesh, world matrix, resolved
///     materials) into the four flat GPU input arrays — deduping meshes into a
///     descriptor table and flattening per-object material slots.
///   - `MeshCuller` (device): owns the GPU buffers + the compute pipeline;
///     uploads the tables, dispatches the cull, and exposes the output
///     instance/indirect/count buffers for `MeshPass::SubmitIndirect`.
///
/// F1 deliberately does frustum-only culling, LOD0 only, and one command per
/// surviving submesh (no cross-object instance coalescing). The CPU extract/sort
/// path (stages A5–E) stays behind a runtime toggle as the pixel-exact reference
/// to validate this against. GPU instance coalescing, screen-size LOD selection,
/// and a dirty-tracked ECS→GPU object mirror (so the per-frame CPU table build
/// disappears too) are stage F2. See docs/mesh-material-architecture.md §9.

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Buffer.hpp>
#include <Assisi/Render/ComputeShader.hpp>

namespace Assisi::Render
{
class MeshBuffer;
class Material;

// ---- GPU input structs (std430) — must match mesh_cull.comp's Object/MeshDesc/
// SubMesh. All vec3 data rides in vec4 for std430 alignment. -------------------

/// @brief One scene object: an entity's placed mesh. `model` is its world
/// matrix; `meshDescIndex` selects its geometry; the material slice
/// [materialBase, materialBase+materialCount) resolves each submesh's slot.
struct GpuObject
{
    glm::mat4 model{1.f};
    uint32_t  meshDescIndex = 0;
    uint32_t  materialBase  = 0; ///< Into CullTables::objectMaterials.
    uint32_t  materialCount = 0; ///< == the mesh's material-slot count.
    uint32_t  _pad0         = 0;
};
static_assert(sizeof(GpuObject) == 80, "GpuObject must match mesh_cull.comp's std430 Object.");

/// @brief One mesh's geometry record: arena base offsets, its LOD0 submesh range
/// (into CullTables::submeshes), and local-space bounds for culling.
struct GpuMeshDesc
{
    glm::vec4 sphere{0.f};  ///< xyz = local center, w = radius.
    glm::vec4 aabbMin{0.f}; ///< xyz = local AABB min.
    glm::vec4 aabbMax{0.f}; ///< xyz = local AABB max.
    uint32_t  vertexBase   = 0;
    uint32_t  indexBase    = 0;
    uint32_t  firstSubmesh = 0; ///< Into CullTables::submeshes (LOD0 first submesh).
    uint32_t  submeshCount = 0; ///< LOD0 submesh count.
};
static_assert(sizeof(GpuMeshDesc) == 64, "GpuMeshDesc must match mesh_cull.comp's std430 MeshDesc.");

/// @brief One submesh: a mesh-local index range and the material slot it draws.
struct GpuSubMesh
{
    uint32_t indexOffset  = 0; ///< Mesh-local; + meshDesc.indexBase = arena start index.
    uint32_t indexCount   = 0;
    uint32_t materialSlot = 0;
    uint32_t _pad0        = 0;
};
static_assert(sizeof(GpuSubMesh) == 16, "GpuSubMesh must match mesh_cull.comp's std430 SubMesh.");

/// @brief Material index the CPU packs for a slot the MeshRenderer left
/// unresolved; the cull shader skips that submesh instead of drawing it with a
/// wrong material (mirrors the CPU path's `material == nullptr` skip). Must match
/// mesh_cull.comp's NO_MATERIAL.
inline constexpr uint32_t kNoMaterial = 0xFFFFFFFFu;

/// @brief The four flat host arrays the culler uploads, plus the draw upper
/// bound. Built once per frame in F1; F2 dirty-tracks them.
struct CullTables
{
    std::vector<GpuObject>   objects;
    std::vector<GpuMeshDesc> meshDescs;
    std::vector<GpuSubMesh>  submeshes;
    std::vector<uint32_t>    objectMaterials;
    /// Sum of every object's LOD0 submesh count — the maximum draws the cull pass
    /// can emit, so the output instance/indirect buffers and the
    /// drawIndexedIndirectCount cap size to it.
    uint32_t drawCapacity = 0;

    bool Empty() const { return objects.empty(); }
    void Clear();
};

/// @brief A mesh's geometry as the culler needs it — local bounds, arena base
/// offsets, and its LOD0 submeshes. The POD the descriptor table is packed from,
/// so the packing core (AddInstanceRaw) is device-free and unit-testable.
struct MeshGeometry
{
    glm::vec4 sphere{0.f};  ///< xyz = local center, w = radius.
    glm::vec4 aabbMin{0.f}; ///< xyz = local AABB min.
    glm::vec4 aabbMax{0.f}; ///< xyz = local AABB max.
    uint32_t  vertexBase = 0;
    uint32_t  indexBase  = 0;
    std::span<const GpuSubMesh> lod0Submeshes; ///< LOD0 submeshes, in draw order.
};

/// @brief Accumulates a frame's @ref CullTables from per-instance inputs, deduping
/// meshes into the descriptor table. The packing core (@ref AddInstanceRaw) is
/// device-free so it is unit-testable; the @ref MeshBuffer overload adapts a
/// resolved mesh + materials onto it.
class CullTableBuilder
{
  public:
    /// @brief Drops all accumulated tables and the mesh dedup map for a new frame.
    void Reset();

    /// @brief Packing core (no device types). Interns @p meshKey's @p geometry into
    /// the descriptor table on first sight (reusing it for later instances of the
    /// same key), then appends one object at @p model whose material slots are
    /// @p materialIds (element i = slot i's Material::Id, or kNoMaterial when
    /// unresolved → that submesh is skipped by the cull shader). No-op if @p geometry
    /// has no LOD0 submeshes. @p meshKey is any stable per-mesh identity.
    void AddInstanceRaw(const void *meshKey, const MeshGeometry &geometry, const glm::mat4 &model,
                        std::span<const uint32_t> materialIds);

    /// @brief Adds one object drawing @p mesh at @p model, resolving each of the
    /// mesh's material slots from @p slotMaterials (element i = slot i's Material,
    /// or null/short = unresolved → skipped). Extracts @p mesh's LOD0 geometry and
    /// its materials' ids and delegates to AddInstanceRaw. No-op if @p mesh is null
    /// or has no LOD0 submeshes.
    void AddInstance(const MeshBuffer *mesh, const glm::mat4 &model,
                     std::span<const Material *const> slotMaterials);

    [[nodiscard]] const CullTables &Tables() const { return _tables; }

  private:
    CullTables                                _tables;
    std::unordered_map<const void *, uint32_t> _meshIndex;

    // Reused scratch so the MeshBuffer overload doesn't allocate per instance:
    // the extracted LOD0 submeshes and material ids fed to AddInstanceRaw.
    std::vector<GpuSubMesh> _submeshScratch;
    std::vector<uint32_t>   _materialScratch;
};

/// @brief Owns the GPU buffers + compute pipeline for the mesh cull pass and runs
/// it. Buffers grow geometrically; a growth rebuilds the cull binding set (and,
/// via the swapped instance-buffer handle, the mesh pass's global set).
class MeshCuller
{
  public:
    MeshCuller() = default;

    /// @brief Loads mesh_cull.comp and allocates the initial buffers.
    /// @return false if the compute shader failed to build.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device);

    /// @brief Uploads @p tables, clears the draw counter, and dispatches the cull
    /// (one thread per object). @p frustumCull=false skips the frustum test so
    /// every object survives — the GPU-path analogue of the CPU cull toggle.
    /// No-op when @p tables is empty. After this call the output buffers hold the
    /// frame's draws for MeshPass::SubmitIndirect.
    void Cull(nvrhi::ICommandList *commandList, const std::array<glm::vec4, 6> &frustumPlanes,
              const CullTables &tables, bool frustumCull);

    /// @brief The instance-data buffer the cull pass wrote (UAV) and the mesh pass
    /// reads by gl_InstanceIndex (SRV). Handle stable unless a growth swapped it.
    [[nodiscard]] nvrhi::IBuffer *InstanceBuffer() const { return _instanceBuffer.NativeBuffer(); }
    /// @brief The GPU-built DrawIndexedIndirectArguments buffer.
    [[nodiscard]] nvrhi::IBuffer *IndirectBuffer() const { return _indirectBuffer; }
    /// @brief The single-uint draw-count buffer (drawIndexedIndirectCount's count).
    [[nodiscard]] nvrhi::IBuffer *CountBuffer() const { return _countBuffer; }
    /// @brief The maxDrawCount cap for the last Cull (== tables.drawCapacity).
    [[nodiscard]] uint32_t MaxDrawCount() const { return _lastMaxDraws; }

    /// @brief The number of draws the cull pass actually emitted (survivors),
    /// read back from the GPU count buffer with a few frames' latency — so culling
    /// is observable in the overlay (it falls below MaxDrawCount as geometry is
    /// culled). Reports MaxDrawCount until the readback ring is primed.
    [[nodiscard]] uint32_t SurvivorDrawCount() const;

    [[nodiscard]] bool IsValid() const { return _cullShader.IsValid(); }

  private:
    /// @brief Grows @p buffer to hold @p neededElements of @p stride bytes if it
    /// can't already; sets @p _bindingSetDirty on a (re)allocation.
    void EnsureInput(Buffer &buffer, uint32_t stride, uint32_t neededElements, const char *debugName);
    /// @brief Grows the output instance buffer (UAV) to @p neededElements records.
    void EnsureInstanceCapacity(uint32_t neededElements);
    /// @brief Grows the indirect + count buffers to hold @p neededDraws commands.
    void EnsureIndirectCapacity(uint32_t neededDraws);
    /// @brief (Re)builds the cull binding set from the current buffer handles.
    void RebuildBindingSet();

    nvrhi::IDevice *_device = nullptr;
    ComputeShader   _cullShader;

    // Input SRV buffers (host-uploaded each Cull).
    Buffer _objectBuffer;
    Buffer _meshDescBuffer;
    Buffer _submeshBuffer;
    Buffer _objectMaterialBuffer;

    // Output buffers. The instance buffer is a structured UAV (compute) + SRV
    // (mesh pass). The indirect + count buffers are structured UAVs (compute
    // atomic append) that are also drawIndirectArgs, auto-state-tracked so NVRHI
    // transitions them UAV→IndirectArgument for the draw.
    Buffer              _instanceBuffer;
    nvrhi::BufferHandle _indirectBuffer;
    nvrhi::BufferHandle _countBuffer;
    uint32_t            _indirectCapacity = 0; // in commands

    nvrhi::BindingSetHandle _cullBindingSet;
    bool                    _bindingSetDirty = true;

    uint32_t _lastMaxDraws = 0;

    // GPU→CPU readback of the survivor draw count, for the overlay. A small ring
    // of CPU-readable buffers: each frame copies _countBuffer into the current
    // slot and maps the slot from kReadbackFrames ago (safely retired) — so the
    // count is a few frames stale but never stalls the GPU. Ring depth ≥ the
    // frames the swapchain keeps in flight.
    static constexpr uint32_t kReadbackFrames = 3;
    nvrhi::BufferHandle       _countReadback[kReadbackFrames];
    uint32_t                  _readbackCursor    = 0;
    uint32_t                  _readbackPrimed    = 0; // writes so far; < kReadbackFrames = ring not yet safe to read
    uint32_t                  _lastSurvivorCount = 0;
};

} // namespace Assisi::Render
