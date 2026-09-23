/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/ClusterGrid.hpp>

#include <Assisi/Core/Logger.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Assisi::Render
{

namespace
{
// Mirrors cluster_build.comp's push_constant block exactly (std430-style
// layout: mat4/uvec4/vec4 are all naturally 16-byte aligned, so no manual
// padding is needed between members).
struct BuildPushConstants
{
    glm::mat4 invProjection;
    glm::uvec4 gridDim;           // xyz used, w unused
    glm::vec4 screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
};
static_assert(sizeof(BuildPushConstants) == 96);

// Mirrors cluster_cull.comp's push_constant block.
struct CullPushConstants
{
    glm::mat4 view;
    glm::uvec4 gridDim;      // xyz used, w unused
    glm::uvec4 lightCounts;  // x = point count, y = spot count, zw unused
};
static_assert(sizeof(CullPushConstants) == 96);

// AABB struct size (2 x vec4) and LightGrid struct size (4 x uint32) —
// must match cluster_build.comp / cluster_cull.comp exactly.
constexpr uint32_t kAABBStride      = 32u;
constexpr uint32_t kLightGridStride = 16u;
constexpr uint32_t kUintStride      = 4u;

// Point and spot lights, each culled into its own half of the index list and
// counted by its own word of the global count.
constexpr uint32_t kCulledLightKinds = 2u;

// Where each of the light cull's buffers sits in its register space. Each must
// match the layout(binding = …) cluster_cull.comp declares it at; the backend
// offsets UAVs into their own range, so a read and a write slot of the same
// number are different bindings.
enum class CullReadSlot : std::uint8_t
{
    ClusterBounds = 0,
    PointLights = 1,
    SpotLights = 2,
};

enum class CullWriteSlot : std::uint8_t
{
    LightIndices = 0,
    LightGrids = 1,
    GlobalCount = 2,
};
} // namespace

bool ClusterGrid::Initialize(nvrhi::IDevice *device)
{
    _device = device;

    nvrhi::BindingLayoutDesc buildLayoutDesc;
    buildLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    buildLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    buildLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BuildPushConstants)));
    if (!_buildShader.Initialize(device, "shaders/cluster_build.comp.spv", buildLayoutDesc))
    {
        Assisi::Core::Log::Error("ClusterGrid: failed to build the cluster_build compute pipeline.");
        return false;
    }

    nvrhi::BindingLayoutDesc cullLayoutDesc;
    cullLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    cullLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(CullReadSlot::ClusterBounds)));
    cullLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(CullReadSlot::PointLights)));
    cullLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(CullReadSlot::SpotLights)));
    cullLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(std::to_underlying(CullWriteSlot::LightIndices)));
    cullLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(std::to_underlying(CullWriteSlot::LightGrids)));
    cullLayoutDesc.addItem(
            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(std::to_underlying(CullWriteSlot::GlobalCount)));
    cullLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(CullPushConstants)));
    if (!_cullShader.Initialize(device, "shaders/cluster_cull.comp.spv", cullLayoutDesc))
    {
        Assisi::Core::Log::Error("ClusterGrid: failed to build the cluster_cull compute pipeline.");
        return false;
    }

    // Fixed-capacity buffers, allocated once and never resized (see Buffer.hpp).
    _clusterAABBBuffer.Create(device, kAABBStride, kNumClusters, /*allowUnorderedAccess=*/ true,
                              "ClusterGrid::ClusterAABBs");
    _pointLightBuffer.Create(device, sizeof(PointLightGPU), kMaxPointLights, false, "ClusterGrid::PointLights");
    _spotLightBuffer.Create(device, sizeof(SpotLightGPU), kMaxSpotLights, false, "ClusterGrid::SpotLights");
    _dirLightBuffer.Create(device, sizeof(DirLightGPU), kMaxDirLights, false, "ClusterGrid::DirLights");
    _lightIndexBuffer.Create(device, kUintStride, kMaxLightIndices * kCulledLightKinds, true, "ClusterGrid::LightIndexList");
    _lightGridBuffer.Create(device, kLightGridStride, kNumClusters, true, "ClusterGrid::LightGrids");
    _globalCountBuffer.Create(device, kUintStride, kCulledLightKinds, true, "ClusterGrid::GlobalCount");

    nvrhi::BindingSetDesc buildSetDesc;
    buildSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, _clusterAABBBuffer.NativeBuffer()));
    buildSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(BuildPushConstants)));
    _buildBindingSet = device->createBindingSet(buildSetDesc, _buildShader.BindingLayout());

    nvrhi::BindingSetDesc cullSetDesc;
    cullSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(CullReadSlot::ClusterBounds),
                                                                    _clusterAABBBuffer.NativeBuffer()));
    cullSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(CullReadSlot::PointLights),
                                                                    _pointLightBuffer.NativeBuffer()));
    cullSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(std::to_underlying(CullReadSlot::SpotLights),
                                                                    _spotLightBuffer.NativeBuffer()));
    cullSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(std::to_underlying(CullWriteSlot::LightIndices),
                                                                    _lightIndexBuffer.NativeBuffer()));
    cullSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(std::to_underlying(CullWriteSlot::LightGrids),
                                                                    _lightGridBuffer.NativeBuffer()));
    cullSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(std::to_underlying(CullWriteSlot::GlobalCount),
                                                                    _globalCountBuffer.NativeBuffer()));
    cullSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(CullPushConstants)));
    _cullBindingSet = device->createBindingSet(cullSetDesc, _cullShader.BindingLayout());

    if (_buildBindingSet == nullptr || _cullBindingSet == nullptr)
    {
        Assisi::Core::Log::Error("ClusterGrid: failed to create the cluster binding sets.");
        return false;
    }
    return true;
}

void ClusterGrid::BuildClusters(nvrhi::ICommandList *commandList, int32_t width, int32_t height, float nearZ,
                                float farZ, const glm::mat4 &invProjection)
{
    _nearZ  = nearZ;
    _farZ   = farZ;
    _width  = width;
    _height = height;

    BuildPushConstants pc;
    pc.invProjection = invProjection;
    pc.gridDim = glm::uvec4(kNumX, kNumY, kNumZ, 0u);
    pc.screenSizeNearFar = glm::vec4(static_cast<float>(width), static_cast<float>(height), nearZ, farZ);

    // One thread per cluster, 64 per workgroup (see cluster_build.comp).
    _buildShader.Dispatch(commandList, _buildBindingSet, kClusterDispatchGroups, 1u, 1u, &pc, sizeof(pc));
}

void ClusterGrid::CullLights(nvrhi::ICommandList *commandList, const std::vector<PointLightGPU> &pointLights,
                             const std::vector<SpotLightGPU> &spotLights, const std::vector<DirLightGPU> &dirLights,
                             const glm::mat4 &view)
{
    _pointLightBuffer.Upload(commandList, pointLights.data(), static_cast<uint32_t>(pointLights.size()));
    _spotLightBuffer.Upload(commandList, spotLights.data(), static_cast<uint32_t>(spotLights.size()));
    _dirLightBuffer.Upload(commandList, dirLights.data(), static_cast<uint32_t>(dirLights.size()));

    _globalCountBuffer.ClearToZero(commandList);

    CullPushConstants pc;
    pc.view = view;
    pc.gridDim = glm::uvec4(kNumX, kNumY, kNumZ, 0u);
    // Buffer::Upload truncates to capacity, so the counts sent to the shader
    // must agree with what was actually uploaded — an un-clamped count makes
    // the cull loop read past the buffer end (robustBufferAccess is off).
    pc.lightCounts = glm::uvec4(std::min(static_cast<uint32_t>(pointLights.size()), kMaxPointLights),
                                std::min(static_cast<uint32_t>(spotLights.size()), kMaxSpotLights), 0u, 0u);

    // One thread per cluster, 64 per workgroup (see cluster_cull.comp).
    _cullShader.Dispatch(commandList, _cullBindingSet, kClusterDispatchGroups, 1u, 1u, &pc, sizeof(pc));
}

} // namespace Assisi::Render
