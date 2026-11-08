/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/WaterRenderer.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/SceneDepth.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace Assisi::Render
{
namespace
{

/// water_spectrum.comp's and water_mips.comp's workgroups are this many texels
/// on a side; the FFT's are one row or column each.
constexpr uint32_t kWaterGroupSide = 16u;
constexpr uint32_t kWaterMipGroupSide = 8u;

constexpr uint32_t kPatchVertexSide = kWaterPatchQuads + 1u;
constexpr uint32_t kIndicesPerQuad = 6u;
constexpr uint32_t kPatchIndexCount = kWaterPatchQuads * kWaterPatchQuads * kIndicesPerQuad;

/// Where an endless body's rectangle reaches: past any draw distance.
constexpr float kEndlessBounds = 1.0e9f;

/// A node buffer grows by this factor, so a growing view reallocates rarely.
constexpr size_t kNodeGrowth = 2u;
constexpr size_t kMinNodeCapacity = 256u;

static_assert(sizeof(WaterSpectrumConstants) == 112u, "Must match water_spectrum.comp's push constants.");
static_assert(sizeof(WaterRowConstants) == 24u, "Must match water_fft_rows.comp's push constants.");
static_assert(sizeof(WaterDrawConstants) == 128u, "Must match water/surface.glsl's WaterConstants.");
static_assert(sizeof(WaterNode) == 16u, "Must match water/surface.glsl's WaterNode.");

nvrhi::TextureHandle MakeWaveTexture(nvrhi::IDevice *device, nvrhi::Format format, uint32_t mips, const char *name)
{
    nvrhi::TextureDesc desc;
    desc.width = kWaterFftSize;
    desc.height = kWaterFftSize;
    desc.arraySize = kWaterCascadeCount;
    desc.mipLevels = mips;
    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.format = format;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = name;
    return device->createTexture(desc);
}

nvrhi::TextureSubresourceSet Mip(uint32_t mip)
{
    return nvrhi::TextureSubresourceSet(mip, 1u, 0u, nvrhi::TextureSubresourceSet::AllArraySlices);
}

glm::vec4 PerCascade(const std::array<float, kWaterCascadeCount> &values)
{
    return glm::vec4(values[0], values[1], values[2], values[3]);
}

glm::vec4 ShapeOf(const WaveSpectrum &spectrum)
{
    return glm::vec4(spectrum.peakFrequency, spectrum.alpha, spectrum.youth, glm::radians(spectrum.bearingDegrees));
}

} // namespace

WaterDrawConstants MakeWaterDrawConstants(const WaterBody &body, glm::dvec3 camera)
{
    WaterDrawConstants constants;
    const glm::dvec2 cameraAcross(camera.x, camera.z);
    std::array<glm::vec2, kWaterCascadeCount> offsets{};
    for (uint32_t c = 0; c < kWaterCascadeCount; ++c)
    {
        offsets[c] = CascadeOffset(cameraAcross, static_cast<double>(body.cascades.tileLength[c]));
    }
    constants.cascadeOffsets = {glm::vec4(offsets[0], offsets[1]), glm::vec4(offsets[2], offsets[3])};
    constants.tileLength = PerCascade(body.cascades.tileLength);
    constants.axes = glm::vec4(body.region.axisX, 0.0f, 0.0f);

    const bool endless = body.region.halfExtent.x <= 0.0f || body.region.halfExtent.y <= 0.0f;
    if (endless)
    {
        constants.bounds = glm::vec4(-kEndlessBounds, -kEndlessBounds, kEndlessBounds, kEndlessBounds);
    }
    else
    {
        const glm::dvec2 cameraInBody = RegionPosition(body.region, camera);
        const glm::dvec2 half(body.region.halfExtent);
        constants.bounds = glm::vec4(glm::vec2(-half - cameraInBody), glm::vec2(half - cameraInBody));
    }

    constants.surface = glm::vec4(static_cast<float>(body.region.origin.y - camera.y), body.significantHeight,
                                  kUnresolvedWaterSlopeVariance, kWaterShoreFade);
    constants.color = glm::vec4(body.look.color, body.look.clarity);
    constants.crestColor = glm::vec4(body.look.crestColor, 0.0f);
    return constants;
}

WaterSpectrumConstants MakeWaterSpectrumConstants(const WaterBody &body)
{
    const WaveSpectrum wind = SpectrumOf(body.waves.wind);
    const WaveSpectrum swell = SpectrumOf(body.waves.swell);
    return WaterSpectrumConstants{.tileLength = PerCascade(body.cascades.tileLength),
                                  .minWavenumber = PerCascade(body.cascades.minWavenumber),
                                  .maxWavenumber = PerCascade(body.cascades.maxWavenumber),
                                  .wind = ShapeOf(wind),
                                  .swell = ShapeOf(swell),
                                  .spreads = glm::vec4(wind.spread, wind.swell, swell.spread, swell.swell),
                                  .seed = glm::uvec4(body.waves.seed, 0u, 0u, 0u)};
}

bool WaterRenderer::Initialize(const InitParams &params)
{
    _device = params.device;
    _frameLayout = params.frameLayout;
    if (_device == nullptr || _frameLayout == nullptr)
    {
        Core::Log::Error("WaterRenderer: no device or frame layout to build on.");
        return false;
    }

    using nvrhi::BindingLayoutItem;
    nvrhi::BindingLayoutDesc spectrumLayout;
    spectrumLayout.visibility = nvrhi::ShaderType::Compute;
    spectrumLayout.addItem(BindingLayoutItem::Texture_UAV(0));
    spectrumLayout.addItem(BindingLayoutItem::PushConstants(0, sizeof(WaterSpectrumConstants)));

    nvrhi::BindingLayoutDesc rowsLayout;
    rowsLayout.visibility = nvrhi::ShaderType::Compute;
    rowsLayout.addItem(BindingLayoutItem::Texture_SRV(0));
    rowsLayout.addItem(BindingLayoutItem::Texture_UAV(0));
    rowsLayout.addItem(BindingLayoutItem::Texture_UAV(1));
    rowsLayout.addItem(BindingLayoutItem::PushConstants(0, sizeof(WaterRowConstants)));

    nvrhi::BindingLayoutDesc columnsLayout;
    columnsLayout.visibility = nvrhi::ShaderType::Compute;
    columnsLayout.addItem(BindingLayoutItem::Texture_SRV(0));
    columnsLayout.addItem(BindingLayoutItem::Texture_SRV(1));
    columnsLayout.addItem(BindingLayoutItem::Texture_UAV(0));
    columnsLayout.addItem(BindingLayoutItem::Texture_UAV(1));

    nvrhi::BindingLayoutDesc mipsLayout = columnsLayout;
    mipsLayout.addItem(BindingLayoutItem::PushConstants(0, sizeof(WaterMipConstants)));

    if (!_spectrumShader.Initialize(_device, params.spectrumSpvPath, spectrumLayout) ||
        !_rowsShader.Initialize(_device, params.rowsSpvPath, rowsLayout) ||
        !_columnsShader.Initialize(_device, params.columnsSpvPath, columnsLayout) ||
        !_mipsShader.Initialize(_device, params.mipsSpvPath, mipsLayout))
    {
        Core::Log::Error("WaterRenderer: a wave compute pipeline failed to build.");
        return false;
    }

    _vertexShader = LoadSpirvShader(_device, params.vertexSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader = LoadSpirvShader(_device, params.pixelSpvPath, nvrhi::ShaderType::Pixel);
    if (_vertexShader == nullptr || _pixelShader == nullptr)
    {
        Core::Log::Error("WaterRenderer: the surface shaders failed to load.");
        return false;
    }

    nvrhi::BindingLayoutDesc drawLayout;
    drawLayout.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    drawLayout.addItem(BindingLayoutItem::ConstantBuffer(0));
    drawLayout.addItem(BindingLayoutItem::Texture_SRV(0));
    drawLayout.addItem(BindingLayoutItem::Texture_SRV(1));
    drawLayout.addItem(BindingLayoutItem::Texture_SRV(2));
    drawLayout.addItem(BindingLayoutItem::StructuredBuffer_SRV(3));
    drawLayout.addItem(BindingLayoutItem::Sampler(0));
    _drawLayout = _device->createBindingLayout(drawLayout);

    // Trilinear and anisotropic, because the slopes' mips are their roughness
    // and the sea is seen at grazing angles; repeating, because the tiles do.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Repeat);
    if (const Vulkan::VulkanContext *context = RenderSystem::GetVulkanContext())
    {
        samplerDesc.maxAnisotropy = context->GetMaxAnisotropy();
    }
    _sampler = _device->createSampler(samplerDesc);

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(WaterDrawConstants);
    constantsDesc.isConstantBuffer = true;
    constantsDesc.debugName = "WaterRenderer::Constants";
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    _constants = _device->createBuffer(constantsDesc);

    if (_drawLayout == nullptr || _sampler == nullptr || _constants == nullptr || !CreatePatch())
    {
        Core::Log::Error("WaterRenderer: the surface's layout, sampler or buffers failed to create.");
        return false;
    }
    return true;
}

bool WaterRenderer::RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (_device == nullptr || _drawLayout == nullptr)
    {
        return false;
    }
    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.VS = _vertexShader;
    desc.PS = _pixelShader;
    desc.addBindingLayout(_frameLayout); // set 0: the mesh pass's frame
    desc.addBindingLayout(_drawLayout);  // set 1: the body's
    // Wound counter-clockwise seen from above, the engine's front.
    desc.renderState.rasterState.frontCounterClockwise = true;
    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    // Tested against the solid scene, writing nothing: the see-through
    // surfaces after it still draw behind and in front of it.
    desc.renderState.depthStencilState.depthTestEnable = true;
    desc.renderState.depthStencilState.depthWriteEnable = false;
    desc.renderState.depthStencilState.depthFunc = kSceneDepthNearerOrSame;
    // What the water sends toward the eye is added, and what is behind it is
    // dimmed by the shader's second output, a colour: shallows take the
    // water's hue as red goes first.
    nvrhi::BlendState::RenderTarget &blend = desc.renderState.blendState.targets[0];
    blend.blendEnable = true;
    blend.srcBlend = nvrhi::BlendFactor::One;
    blend.destBlend = nvrhi::BlendFactor::Src1Color;
    blend.srcBlendAlpha = nvrhi::BlendFactor::One;
    blend.destBlendAlpha = nvrhi::BlendFactor::Src1Alpha;
    _pipeline = _device->createGraphicsPipeline(desc, framebufferInfo);
    if (_pipeline == nullptr)
    {
        Core::Log::Error("WaterRenderer: the surface pipeline failed to build.");
        return false;
    }
    return true;
}

bool WaterRenderer::CreatePatch()
{
    std::vector<uint16_t> indices;
    indices.reserve(kPatchIndexCount);
    for (uint32_t z = 0; z < kWaterPatchQuads; ++z)
    {
        for (uint32_t x = 0; x < kWaterPatchQuads; ++x)
        {
            const uint16_t corner = static_cast<uint16_t>(z * kPatchVertexSide + x);
            const uint16_t alongX = static_cast<uint16_t>(corner + 1u);
            const uint16_t alongZ = static_cast<uint16_t>(corner + kPatchVertexSide);
            const uint16_t across = static_cast<uint16_t>(alongZ + 1u);
            // Counter-clockwise seen from above.
            indices.insert(indices.end(), {corner, alongZ, across, corner, across, alongX});
        }
    }

    nvrhi::BufferDesc desc;
    desc.byteSize = indices.size() * sizeof(uint16_t);
    desc.isIndexBuffer = true;
    desc.debugName = "WaterRenderer::PatchIndices";
    desc.initialState = nvrhi::ResourceStates::IndexBuffer;
    desc.keepInitialState = true;
    _patchIndices = _device->createBuffer(desc);
    if (_patchIndices == nullptr)
    {
        return false;
    }
    nvrhi::CommandListHandle upload = _device->createCommandList();
    upload->open();
    upload->writeBuffer(_patchIndices, indices.data(), desc.byteSize);
    upload->close();
    _device->executeCommandList(upload);
    return true;
}

bool WaterRenderer::CreateScratch()
{
    _scratch01 = MakeWaveTexture(_device, nvrhi::Format::RGBA32_FLOAT, 1u, "WaterRenderer::Scratch01");
    _scratch23 = MakeWaveTexture(_device, nvrhi::Format::RGBA32_FLOAT, 1u, "WaterRenderer::Scratch23");
    if (_scratch01 == nullptr || _scratch23 == nullptr)
    {
        _scratch01 = nullptr;
        _scratch23 = nullptr;
        return false;
    }
    return true;
}

bool WaterRenderer::CreateField(Field &field)
{
    field.spectrum = MakeWaveTexture(_device, nvrhi::Format::RGBA32_FLOAT, 1u, "WaterRenderer::Spectrum");
    field.displacement = MakeWaveTexture(_device, nvrhi::Format::RGBA16_FLOAT, kWaterMipCount, "WaterRenderer::Displacement");
    field.slopes = MakeWaveTexture(_device, nvrhi::Format::RGBA16_FLOAT, kWaterMipCount, "WaterRenderer::Slopes");
    if (field.spectrum == nullptr || field.displacement == nullptr || field.slopes == nullptr)
    {
        return false;
    }

    using nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc spectrumSet;
    spectrumSet.addItem(BindingSetItem::Texture_UAV(0, field.spectrum, nvrhi::Format::UNKNOWN, Mip(0u)));
    spectrumSet.addItem(BindingSetItem::PushConstants(0, sizeof(WaterSpectrumConstants)));
    field.spectrumSet = _device->createBindingSet(spectrumSet, _spectrumShader.BindingLayout());

    nvrhi::BindingSetDesc rowsSet;
    rowsSet.addItem(BindingSetItem::Texture_SRV(0, field.spectrum));
    rowsSet.addItem(BindingSetItem::Texture_UAV(0, _scratch01, nvrhi::Format::UNKNOWN, Mip(0u)));
    rowsSet.addItem(BindingSetItem::Texture_UAV(1, _scratch23, nvrhi::Format::UNKNOWN, Mip(0u)));
    rowsSet.addItem(BindingSetItem::PushConstants(0, sizeof(WaterRowConstants)));
    field.rowsSet = _device->createBindingSet(rowsSet, _rowsShader.BindingLayout());

    nvrhi::BindingSetDesc columnsSet;
    columnsSet.addItem(BindingSetItem::Texture_SRV(0, _scratch01));
    columnsSet.addItem(BindingSetItem::Texture_SRV(1, _scratch23));
    columnsSet.addItem(BindingSetItem::Texture_UAV(0, field.displacement, nvrhi::Format::UNKNOWN, Mip(0u)));
    columnsSet.addItem(BindingSetItem::Texture_UAV(1, field.slopes, nvrhi::Format::UNKNOWN, Mip(0u)));
    field.columnsSet = _device->createBindingSet(columnsSet, _columnsShader.BindingLayout());

    bool built = field.spectrumSet != nullptr && field.rowsSet != nullptr && field.columnsSet != nullptr;
    for (uint32_t mip = 1u; mip < kWaterMipCount; ++mip)
    {
        nvrhi::BindingSetDesc mipSet;
        mipSet.addItem(BindingSetItem::Texture_SRV(0, field.displacement, nvrhi::Format::UNKNOWN, Mip(mip - 1u)));
        mipSet.addItem(BindingSetItem::Texture_SRV(1, field.slopes, nvrhi::Format::UNKNOWN, Mip(mip - 1u)));
        mipSet.addItem(BindingSetItem::Texture_UAV(0, field.displacement, nvrhi::Format::UNKNOWN, Mip(mip)));
        mipSet.addItem(BindingSetItem::Texture_UAV(1, field.slopes, nvrhi::Format::UNKNOWN, Mip(mip)));
        mipSet.addItem(BindingSetItem::PushConstants(0, sizeof(WaterMipConstants)));
        field.mipSets[mip - 1u] = _device->createBindingSet(mipSet, _mipsShader.BindingLayout());
        built = built && field.mipSets[mip - 1u] != nullptr;
    }
    return built;
}

WaterRenderer::Field *WaterRenderer::FieldFor(uint64_t key)
{
    const std::unordered_map<uint64_t, Field>::iterator found = _fields.find(key);
    if (found != _fields.end())
    {
        return &found->second;
    }
    Field field;
    if (!CreateField(field))
    {
        Core::Log::Warn("WaterRenderer: a body of water's wave textures failed to allocate.");
        return nullptr;
    }
    return &_fields.emplace(key, std::move(field)).first->second;
}

bool WaterRenderer::Simulate(nvrhi::ICommandList *commandList, const WaterFrame &frame, double seconds)
{
    if (!IsValid())
    {
        return false;
    }
    for (std::pair<const uint64_t, Field> &entry : _fields)
    {
        ++entry.second.idleFrames;
    }
    if (_scratch01 == nullptr && !frame.bodies.empty() && !CreateScratch())
    {
        Core::Log::Warn("WaterRenderer: the FFT's scratch textures failed to allocate.");
        return false;
    }

    const float loopSeconds = static_cast<float>(WaterLoopTime(seconds));
    for (const WaterBody &body : frame.bodies)
    {
        Field *field = FieldFor(body.key);
        if (field == nullptr)
        {
            return false;
        }
        field->idleFrames = 0u;
        if (!field->spectrumWritten || !(field->waves == body.waves) || !(field->cascades == body.cascades))
        {
            const WaterSpectrumConstants constants = MakeWaterSpectrumConstants(body);
            constexpr uint32_t kGroups = kWaterFftSize / kWaterGroupSide;
            _spectrumShader.Dispatch(commandList, field->spectrumSet, kGroups, kGroups, kWaterCascadeCount, &constants,
                                     sizeof(constants));
            field->waves = body.waves;
            field->cascades = body.cascades;
            field->spectrumWritten = true;
        }
        SimulateField(commandList, *field, body, loopSeconds);
    }

    std::erase_if(_fields, [](const std::pair<const uint64_t, Field> &entry) {
            return entry.second.idleFrames > kWaterIdleFrames;
        });
    if (_fields.empty())
    {
        _scratch01 = nullptr;
        _scratch23 = nullptr;
    }
    return true;
}

void WaterRenderer::SimulateField(nvrhi::ICommandList *commandList, Field &field, const WaterBody &body,
                                  float loopSeconds)
{
    const WaterRowConstants rows{.tileLength = PerCascade(body.cascades.tileLength),
                                 .loopSeconds = loopSeconds,
                                 .choppiness = body.waves.choppiness};
    _rowsShader.Dispatch(commandList, field.rowsSet, kWaterFftSize, kWaterCascadeCount, 1u, &rows, sizeof(rows));
    _columnsShader.Dispatch(commandList, field.columnsSet, kWaterFftSize, kWaterCascadeCount, 1u);
    for (uint32_t mip = 1u; mip < kWaterMipCount; ++mip)
    {
        const uint32_t size = kWaterFftSize >> mip;
        const uint32_t groups = (size + kWaterMipGroupSide - 1u) / kWaterMipGroupSide;
        const WaterMipConstants constants{.size = glm::uvec2(size)};
        _mipsShader.Dispatch(commandList, field.mipSets[mip - 1u], groups, groups, kWaterCascadeCount, &constants,
                             sizeof(constants));
    }
}

bool WaterRenderer::EnsureNodeCapacity(size_t count)
{
    if (_nodes != nullptr && count <= _nodeCapacity)
    {
        return true;
    }
    const size_t capacity = std::max({count, _nodeCapacity * kNodeGrowth, kMinNodeCapacity});
    nvrhi::BufferDesc desc;
    desc.byteSize = capacity * sizeof(WaterNode);
    desc.structStride = sizeof(WaterNode);
    desc.debugName = "WaterRenderer::Nodes";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    _nodes = _device->createBuffer(desc);
    _nodeCapacity = _nodes != nullptr ? capacity : 0u;
    return _nodes != nullptr;
}

void WaterRenderer::Draw(const RenderFrame &target, const WaterFrame &frame, const WaterDrawView &view)
{
    if (!IsValid() || frame.bodies.empty() || view.frameBindings == nullptr || view.sceneDistance == nullptr)
    {
        return;
    }
    if (!EnsureNodeCapacity(frame.nodes.size()))
    {
        Core::Log::Warn("WaterRenderer: the patch buffer failed to allocate.");
        return;
    }
    nvrhi::ICommandList *commandList = target.commandList;
    commandList->writeBuffer(_nodes, frame.nodes.data(), frame.nodes.size() * sizeof(WaterNode));

    using nvrhi::BindingSetItem;
    for (const WaterBody &body : frame.bodies)
    {
        const std::unordered_map<uint64_t, Field>::iterator found = _fields.find(body.key);
        if (found == _fields.end())
        {
            continue;
        }
        Field &field = found->second;
        if (field.drawSet == nullptr || field.drawSetDistance != view.sceneDistance || field.drawSetNodes != _nodes)
        {
            nvrhi::BindingSetDesc set;
            set.addItem(BindingSetItem::ConstantBuffer(0, _constants));
            set.addItem(BindingSetItem::Texture_SRV(0, field.displacement));
            set.addItem(BindingSetItem::Texture_SRV(1, field.slopes));
            set.addItem(BindingSetItem::Texture_SRV(2, view.sceneDistance));
            set.addItem(BindingSetItem::StructuredBuffer_SRV(3, _nodes));
            set.addItem(BindingSetItem::Sampler(0, _sampler));
            field.drawSet = _device->createBindingSet(set, _drawLayout);
            field.drawSetDistance = view.sceneDistance;
            field.drawSetNodes = _nodes;
        }
        if (field.drawSet == nullptr)
        {
            continue;
        }

        const WaterDrawConstants constants = MakeWaterDrawConstants(body, view.camera);
        commandList->writeBuffer(_constants, &constants, sizeof(constants));

        nvrhi::GraphicsState state;
        state.pipeline = _pipeline;
        state.framebuffer = target.framebuffer;
        state.addBindingSet(view.frameBindings);
        state.addBindingSet(field.drawSet);
        state.indexBuffer =
            nvrhi::IndexBufferBinding{.buffer = _patchIndices, .format = nvrhi::Format::R16_UINT, .offset = 0u};
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(target.width), static_cast<float>(target.height)));
        commandList->setGraphicsState(state);

        nvrhi::DrawArguments draw;
        draw.vertexCount = kPatchIndexCount;
        draw.instanceCount = body.nodeCount;
        draw.startInstanceLocation = body.firstNode;
        commandList->drawIndexed(draw);
    }
}

void WaterRenderer::Release()
{
    _fields.clear();
    _scratch01 = nullptr;
    _scratch23 = nullptr;
    _nodes = nullptr;
    _nodeCapacity = 0u;
}

} // namespace Assisi::Render
