/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WaterRenderer.hpp
/// @brief Open water on the GPU: each body's waves brought to the time, then
/// its surface drawn over the finished scene.
///
/// Each body in view keeps a set of wave textures. Once, when it comes into
/// view or its settings change, water_spectrum.comp writes every wave's start.
/// Each frame the waves are moved to the time and summed by an FFT in two
/// passes, rows then columns, into each cascade's displacement and slope
/// textures, whose mips are then averaged down. Draw then lays the patches the
/// gather chose over the scene, blended, as water.vert and water.frag describe.
///
/// A body out of view costs nothing each frame; its textures are freed once it
/// has been out of view for kWaterIdleFrames.

#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/ComputeShader.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Render/WaterFrame.hpp>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Assisi::Render
{

/// @brief Mips of each wave texture, from kWaterFftSize down to one texel.
inline constexpr uint32_t kWaterMipCount = 9u;

/// @brief A body's wave textures are kept this many frames after it was last in
/// view, so turning away and back does not reallocate them.
inline constexpr uint32_t kWaterIdleFrames = 600u;

/// @brief The slope variance, per axis, of ripples finer than any cascade
/// holds: what keeps the sun's highlight from shrinking to a point on calm
/// water.
inline constexpr float kUnresolvedWaterSlopeVariance = 0.0015f;

/// @brief How deep the water must be, in metres, before its edge stops fading:
/// what hides the line where water meets ground.
inline constexpr float kWaterShoreFade = 0.4f;

/// @brief water_spectrum.comp's push constants, field for field.
struct WaterSpectrumConstants
{
    glm::vec4 tileLength{0.0f};
    glm::vec4 minWavenumber{0.0f};
    glm::vec4 maxWavenumber{0.0f};
    /// Peak frequency, alpha, youth, bearing in radians.
    glm::vec4 wind{0.0f};
    glm::vec4 swell{0.0f};
    /// The wind's spread and swell, then the swell's.
    glm::vec4 spreads{0.0f};
    glm::uvec4 seed{0u};
};

/// @brief water_fft_rows.comp's push constants, field for field.
struct WaterRowConstants
{
    glm::vec4 tileLength{0.0f};
    float loopSeconds = 0.0f;
    float choppiness = 0.0f;
};

/// @brief water_mips.comp's push constants.
struct WaterMipConstants
{
    glm::uvec2 size{0u};
};

/// @brief water/surface.glsl's WaterConstants, field for field.
struct WaterDrawConstants
{
    std::array<glm::vec4, 2> cascadeOffsets{};
    glm::vec4 tileLength{0.0f};
    glm::vec4 axes{0.0f};
    glm::vec4 bounds{0.0f};
    glm::vec4 surface{0.0f};
    glm::vec4 color{0.0f};
    glm::vec4 crestColor{0.0f};
};

/// @brief What one view's water is drawn with.
struct WaterDrawView
{
    /// What the lit pass measures from.
    glm::dvec3 camera{0.0};
    /// The mesh pass's frame bindings, set 0 of the surface pipeline.
    nvrhi::IBindingSet *frameBindings = nullptr;
    /// The scene's depth per pixel, as SceneDistancePass makes it.
    nvrhi::ITexture *sceneDistance = nullptr;
};

/// @brief The constants water.vert and water.frag draw @p body with, seen from
/// @p camera.
[[nodiscard]] WaterDrawConstants MakeWaterDrawConstants(const WaterBody &body, glm::dvec3 camera);

/// @brief water_spectrum.comp's constants for @p body.
[[nodiscard]] WaterSpectrumConstants MakeWaterSpectrumConstants(const WaterBody &body);

class WaterRenderer
{
public:
    struct InitParams
    {
        nvrhi::IDevice *device = nullptr;
        /// The mesh pass's set-0 layout (MeshPass::FrameBindingLayout).
        nvrhi::IBindingLayout *frameLayout = nullptr;
        std::string spectrumSpvPath;
        std::string rowsSpvPath;
        std::string columnsSpvPath;
        std::string mipsSpvPath;
        std::string vertexSpvPath;
        std::string pixelSpvPath;
    };

    /// @brief Loads the shaders and builds the wave pipelines. Allocates no wave
    /// textures: those come with the first body in view.
    [[nodiscard]] bool Initialize(const InitParams &params);

    /// @brief Builds the surface pipeline for the scene target, again whenever
    /// it changes format or sample count. Nothing is drawn until it has.
    [[nodiscard]] bool RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo);

    [[nodiscard]] bool IsValid() const { return _pipeline != nullptr; }

    /// @brief Brings every body of @p frame to @p seconds on the shared clock.
    /// @return false if a body's textures failed to allocate; nothing is then
    /// drawn this frame.
    [[nodiscard]] bool Simulate(nvrhi::ICommandList *commandList, const WaterFrame &frame, double seconds);

    /// @brief Draws every body of @p frame into @p target, over what is there.
    /// @pre Simulate this frame, with the same @p frame.
    void Draw(const RenderFrame &target, const WaterFrame &frame, const WaterDrawView &view);

    /// @brief Frees every body's textures and the shared scratch.
    void Release();

    /// @brief How many bodies hold wave textures.
    [[nodiscard]] size_t FieldCount() const { return _fields.size(); }

private:
    /// @brief One body's wave textures, and what its spectrum was written for.
    struct Field
    {
        nvrhi::TextureHandle spectrum;
        nvrhi::TextureHandle displacement;
        nvrhi::TextureHandle slopes;
        nvrhi::BindingSetHandle spectrumSet;
        nvrhi::BindingSetHandle rowsSet;
        nvrhi::BindingSetHandle columnsSet;
        std::array<nvrhi::BindingSetHandle, kWaterMipCount - 1u> mipSets;
        /// The surface's set, and what it was built against.
        nvrhi::BindingSetHandle drawSet;
        const nvrhi::ITexture *drawSetDistance = nullptr;
        const nvrhi::IBuffer *drawSetNodes = nullptr;
        WaterCascades cascades;
        WaterWaves waves;
        uint32_t idleFrames = 0u;
        bool spectrumWritten = false;
    };

    /// @brief @p key's field, made if it has none. Null if it failed to allocate.
    [[nodiscard]] Field *FieldFor(uint64_t key);

    [[nodiscard]] bool CreateField(Field &field);
    [[nodiscard]] bool CreateScratch();
    [[nodiscard]] bool CreatePatch();
    [[nodiscard]] bool EnsureNodeCapacity(size_t count);

    /// @brief The FFT and the mips for one body.
    void SimulateField(nvrhi::ICommandList *commandList, Field &field, const WaterBody &body, float loopSeconds);

    nvrhi::IDevice *_device = nullptr;
    nvrhi::IBindingLayout *_frameLayout = nullptr;

    ComputeShader _spectrumShader;
    ComputeShader _rowsShader;
    ComputeShader _columnsShader;
    ComputeShader _mipsShader;

    nvrhi::ShaderHandle _vertexShader;
    nvrhi::ShaderHandle _pixelShader;
    nvrhi::BindingLayoutHandle _drawLayout;
    nvrhi::GraphicsPipelineHandle _pipeline;
    nvrhi::SamplerHandle _sampler;
    nvrhi::BufferHandle _constants;
    nvrhi::BufferHandle _patchIndices;
    nvrhi::BufferHandle _nodes;
    size_t _nodeCapacity = 0u;

    /// The FFT's halfway results, shared by every body in turn.
    nvrhi::TextureHandle _scratch01;
    nvrhi::TextureHandle _scratch23;

    std::unordered_map<uint64_t, Field> _fields;
};

} // namespace Assisi::Render
