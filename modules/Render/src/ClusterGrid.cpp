/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <glad/glad.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/ClusterGrid.hpp>

#include <algorithm>
#include <cstring>

namespace Assisi::Render
{

// ---- Lifetime --------------------------------------------------------------

ClusterGrid::~ClusterGrid()
{
    Destroy();
}

ClusterGrid::ClusterGrid(ClusterGrid &&o) noexcept
{
    *this = std::move(o);
}

ClusterGrid &ClusterGrid::operator=(ClusterGrid &&o) noexcept
{
    if (this != &o)
    {
        Destroy();
        _buildShader       = std::move(o._buildShader);
        _cullShader        = std::move(o._cullShader);
        _clusterAABBBuffer = o._clusterAABBBuffer; o._clusterAABBBuffer = 0u;
        _pointLightBuffer  = o._pointLightBuffer;  o._pointLightBuffer  = 0u;
        _spotLightBuffer   = o._spotLightBuffer;   o._spotLightBuffer   = 0u;
        _dirLightBuffer    = o._dirLightBuffer;    o._dirLightBuffer    = 0u;
        _lightIndexBuffer  = o._lightIndexBuffer;  o._lightIndexBuffer  = 0u;
        _lightGridBuffer   = o._lightGridBuffer;   o._lightGridBuffer   = 0u;
        _globalCountBuffer = o._globalCountBuffer; o._globalCountBuffer = 0u;
        _nearZ  = o._nearZ;
        _farZ   = o._farZ;
        _width  = o._width;
        _height = o._height;
    }
    return *this;
}

// ---- Initialize / destroy --------------------------------------------------

bool ClusterGrid::Initialize()
{
    _buildShader = ComputeShader("shaders/cluster_build.comp");
    if (!_buildShader.IsValid())
    {
        Assisi::Core::Log::Error("ClusterGrid: failed to compile cluster_build.comp");
        return false;
    }

    _cullShader = ComputeShader("shaders/cluster_cull.comp");
    if (!_cullShader.IsValid())
    {
        Assisi::Core::Log::Error("ClusterGrid: failed to compile cluster_cull.comp");
        return false;
    }

    AllocateBuffers();
    return true;
}

void ClusterGrid::AllocateBuffers()
{
    auto alloc = [](unsigned int &buf, GLsizeiptr bytes)
    {
        if (buf)
            glDeleteBuffers(1, &buf);
        glCreateBuffers(1, &buf);
        glNamedBufferData(buf, bytes, nullptr, GL_DYNAMIC_DRAW);
    };

    // Cluster AABB buffer: 2 × vec4 (32 bytes) per cluster
    alloc(_clusterAABBBuffer, static_cast<GLsizeiptr>(kNumClusters * 32u));

    // Light index list: 2 × kMaxLightIndices uint32s (point first, then spot)
    alloc(_lightIndexBuffer, static_cast<GLsizeiptr>(kMaxLightIndices * 2u * sizeof(unsigned int)));

    // Light grid: 4 × uint32 (16 bytes) per cluster
    alloc(_lightGridBuffer, static_cast<GLsizeiptr>(kNumClusters * 16u));

    // Atomic counters (2 × uint32)
    alloc(_globalCountBuffer, static_cast<GLsizeiptr>(2u * sizeof(unsigned int)));

    // Light data buffers — sized to 1 byte initially; resized on first upload
    auto allocTiny = [](unsigned int &buf)
    {
        if (buf)
            glDeleteBuffers(1, &buf);
        glCreateBuffers(1, &buf);
        glNamedBufferData(buf, 4, nullptr, GL_DYNAMIC_DRAW);
    };
    allocTiny(_pointLightBuffer);
    allocTiny(_spotLightBuffer);
    allocTiny(_dirLightBuffer);
}

void ClusterGrid::Destroy() noexcept
{
    auto del = [](unsigned int &buf)
    {
        if (buf)
        {
            glDeleteBuffers(1, &buf);
            buf = 0u;
        }
    };
    del(_clusterAABBBuffer);
    del(_pointLightBuffer);
    del(_spotLightBuffer);
    del(_dirLightBuffer);
    del(_lightIndexBuffer);
    del(_lightGridBuffer);
    del(_globalCountBuffer);
}

// ---- BuildClusters ---------------------------------------------------------

void ClusterGrid::BuildClusters(int width, int height, float nearZ, float farZ,
                                const glm::mat4 &invProjection)
{
    _nearZ  = nearZ;
    _farZ   = farZ;
    _width  = width;
    _height = height;

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingClusterAABB, _clusterAABBBuffer);

    _buildShader.Use();
    _buildShader.SetUVec3("uGridDim",  kNumX, kNumY, kNumZ);
    _buildShader.SetVec2("uScreenSize", static_cast<float>(width), static_cast<float>(height));
    _buildShader.SetFloat("uNearZ", nearZ);
    _buildShader.SetFloat("uFarZ",  farZ);
    _buildShader.SetMat4("uInvProjection", invProjection);

    glDispatchCompute(kNumX, kNumY, kNumZ);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

// ---- CullLights ------------------------------------------------------------

void ClusterGrid::BindAll() const
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingClusterAABB,  _clusterAABBBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingPointLights,  _pointLightBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingSpotLights,   _spotLightBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingDirLights,    _dirLightBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLightIndex,   _lightIndexBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingLightGrid,    _lightGridBuffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, kBindingGlobalCount,  _globalCountBuffer);
}

void ClusterGrid::CullLights(const std::vector<PointLightGPU> &pointLights,
                             const std::vector<SpotLightGPU>  &spotLights,
                             const std::vector<DirLightGPU>   &dirLights,
                             const glm::mat4                  &view)
{
    // Upload light data (glNamedBufferData reallocates if size changed)
    auto upload = [](unsigned int buf, const void *data, size_t bytes)
    {
        glNamedBufferData(buf, static_cast<GLsizeiptr>(std::max(bytes, size_t{4})), data, GL_DYNAMIC_DRAW);
    };
    upload(_pointLightBuffer, pointLights.data(), pointLights.size() * sizeof(PointLightGPU));
    upload(_spotLightBuffer,  spotLights.data(),  spotLights.size()  * sizeof(SpotLightGPU));
    upload(_dirLightBuffer,   dirLights.data(),   dirLights.size()   * sizeof(DirLightGPU));

    // Reset global atomic counters to zero
    const unsigned int zeros[2] = {0u, 0u};
    glNamedBufferSubData(_globalCountBuffer, 0, sizeof(zeros), zeros);

    // Bind all SSBOs (they stay bound for the subsequent DrawScene call)
    BindAll();

    // Dispatch culling pass
    _cullShader.Use();
    _cullShader.SetUVec3("uGridDim", kNumX, kNumY, kNumZ);
    _cullShader.SetUInt("uPointLightCount", static_cast<unsigned int>(pointLights.size()));
    _cullShader.SetUInt("uSpotLightCount",  static_cast<unsigned int>(spotLights.size()));
    _cullShader.SetMat4("uView", view);

    glDispatchCompute(kNumX, kNumY, kNumZ);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
}

} // namespace Assisi::Render