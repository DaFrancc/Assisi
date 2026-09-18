/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/SsaoPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/ShaderModule.hpp>
#include <Assisi/Render/Ssao.hpp>

#include <algorithm>
#include <vector>

namespace Assisi::Render
{

namespace
{
// A visible fraction in [0, 1]; eight bits is finer than anything the blur leaves.
constexpr nvrhi::Format kOcclusionFormat = nvrhi::Format::R8_UNORM;

// Mirrors the SsaoConstants block every SSAO shader declares.
struct SsaoConstants
{
    /// x = xScale, y = yScale, z = depthScale, w = depthBias (see SsaoProjection).
    glm::vec4 projection;
    /// xy = target size in pixels, z = far plane distance, w = pixels per metre
    /// of radius at one metre from the camera.
    glm::vec4 viewport;
    /// x = radius in metres, y = strength, z = kSsaoMaxRadiusPixels,
    /// w = kSsaoBiasFraction.
    glm::vec4 params;
    /// x = kSsaoBlurDepthTolerance, yzw unused.
    glm::vec4 blur;
    /// x = samples to take, yzw unused.
    glm::uvec4 counts;
    /// xyz = one kernel offset, w unused. Entries past counts.x are unread.
    std::array<glm::vec4, kMaxSsaoSampleCount> kernel;
};

ASSISI_GPU_LAYOUT(SsaoConstants);
ASSISI_GPU_FIRST_FIELD(SsaoConstants, projection);
ASSISI_GPU_FIELD_AFTER(SsaoConstants, viewport, projection);
ASSISI_GPU_FIELD_AFTER(SsaoConstants, params, viewport);
ASSISI_GPU_FIELD_AFTER(SsaoConstants, blur, params);
ASSISI_GPU_FIELD_AFTER(SsaoConstants, counts, blur);
ASSISI_GPU_FIELD_AFTER(SsaoConstants, kernel, counts);
ASSISI_GPU_NO_TAIL_PADDING(SsaoConstants, kernel);

// The blur's axis, as ssao_blur.frag's push constant reads it.
using BlurAxis = glm::ivec2;

nvrhi::FramebufferInfo SingleTargetInfo(nvrhi::Format format)
{
    nvrhi::FramebufferInfo info;
    info.colorFormats.push_back(format);
    return info;
}
} // namespace

bool SsaoPass::Initialize(const InitParams &params)
{
    _device = params.device;

    _vertexShader = LoadSpirvShader(_device, params.vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    const nvrhi::ShaderHandle occlusionShader =
        LoadSpirvShader(_device, params.occlusionShaderSpvPath, nvrhi::ShaderType::Pixel);
    const nvrhi::ShaderHandle blurShader = LoadSpirvShader(_device, params.blurShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !occlusionShader || !blurShader)
    {
        return false;
    }

    nvrhi::BindingLayoutDesc singleInput;
    singleInput.visibility = nvrhi::ShaderType::Pixel;
    singleInput.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    singleInput.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    _singleInputLayout = _device->createBindingLayout(singleInput);

    nvrhi::BindingLayoutDesc blur;
    blur.visibility = nvrhi::ShaderType::Pixel;
    blur.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    blur.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    blur.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    blur.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BlurAxis)));
    _blurLayout = _device->createBindingLayout(blur);

    const auto makePipeline = [this](nvrhi::IShader *pixelShader, nvrhi::IBindingLayout *layout, nvrhi::Format target)
    {
        nvrhi::GraphicsPipelineDesc desc;
        desc.primType = nvrhi::PrimitiveType::TriangleList;
        desc.VS = _vertexShader;
        desc.PS = pixelShader;
        desc.addBindingLayout(layout);
        desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        desc.renderState.depthStencilState.depthTestEnable = false;
        desc.renderState.depthStencilState.depthWriteEnable = false;
        return _device->createGraphicsPipeline(desc, SingleTargetInfo(target));
    };
    _blurPipeline = makePipeline(blurShader, _blurLayout, kOcclusionFormat);
    // Last, because IsValid() reads it: a pass whose other pipelines failed
    // must not report itself usable.
    const nvrhi::GraphicsPipelineHandle occlusion = makePipeline(occlusionShader, _singleInputLayout, kOcclusionFormat);
    if (!_blurPipeline || !occlusion)
    {
        Core::Log::Error("SsaoPass: failed to create the pipelines.");
        return false;
    }

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(SsaoConstants);
    constantsDesc.isConstantBuffer = true;
    constantsDesc.debugName = "SsaoPass::Constants";
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    _constants = _device->createBuffer(constantsDesc);
    if (_constants == nullptr)
    {
        Core::Log::Error("SsaoPass: failed to create the constant buffer.");
        return false;
    }

    _occlusionPipeline = occlusion;
    return true;
}

bool SsaoPass::Configure(uint32_t width, uint32_t height, nvrhi::ITexture *distance)
{
    if (width == 0 || height == 0 || distance == nullptr)
    {
        Release();
        return false;
    }
    const bool resized = width != _width || height != _height || _occlusion == nullptr;
    if (!resized && distance == _distance)
    {
        return true;
    }

    _width = width;
    _height = height;
    _distance = distance;
    if (resized && !CreateTargets())
    {
        Core::Log::Error("SsaoPass: failed to allocate the {}x{} targets.", width, height);
        Release();
        return false;
    }
    CreateBindingSets();
    return true;
}

bool SsaoPass::CreateTargets()
{
    const auto makeTarget = [this](nvrhi::Format format, const char *debugName)
    {
        nvrhi::TextureDesc desc;
        desc.width = _width;
        desc.height = _height;
        desc.format = format;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = debugName;
        return _device->createTexture(desc);
    };
    const auto makeFramebuffer = [this](nvrhi::ITexture *target)
    {
        nvrhi::FramebufferDesc desc;
        desc.addColorAttachment(target);
        return _device->createFramebuffer(desc);
    };

    _occlusion = makeTarget(kOcclusionFormat, "SsaoPass::Occlusion");
    _blurScratch = makeTarget(kOcclusionFormat, "SsaoPass::BlurScratch");
    if (!_occlusion || !_blurScratch)
    {
        return false;
    }
    _occlusionFramebuffer = makeFramebuffer(_occlusion);
    _blurScratchFramebuffer = makeFramebuffer(_blurScratch);
    return _occlusionFramebuffer && _blurScratchFramebuffer;
}

void SsaoPass::CreateBindingSets()
{
    const auto singleInput = [this](nvrhi::ITexture *input)
    {
        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _constants));
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, input));
        return _device->createBindingSet(desc, _singleInputLayout);
    };
    const auto blur = [this](nvrhi::ITexture *input)
    {
        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _constants));
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, input));
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, _distance));
        desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(BlurAxis)));
        return _device->createBindingSet(desc, _blurLayout);
    };

    _bindingSets[static_cast<uint32_t>(Step::Occlusion)] = singleInput(_distance);
    _bindingSets[static_cast<uint32_t>(Step::BlurHorizontal)] = blur(_occlusion);
    _bindingSets[static_cast<uint32_t>(Step::BlurVertical)] = blur(_blurScratch);
}

void SsaoPass::Release()
{
    for (nvrhi::BindingSetHandle &set : _bindingSets)
    {
        set = nullptr;
    }
    _occlusionFramebuffer = nullptr;
    _blurScratchFramebuffer = nullptr;
    _occlusion = nullptr;
    _blurScratch = nullptr;
    _distance = nullptr;
    _width = 0;
    _height = 0;
}

void SsaoPass::Render(nvrhi::ICommandList *commandList, const Frame &frame) const
{
    const SsaoSettings settings = Sanitized(frame.settings);
    const SsaoProjection projection = MakeSsaoProjection(frame.projection);

    SsaoConstants constants{};
    constants.projection =
        glm::vec4(projection.xScale, projection.yScale, projection.depthScale, projection.depthBias);
    constants.viewport = glm::vec4(static_cast<float>(_width), static_cast<float>(_height), frame.farZ,
                                   projection.yScale * 0.5f * static_cast<float>(_height));
    constants.params = glm::vec4(settings.radius, settings.strength, kSsaoMaxRadiusPixels, kSsaoBiasFraction);
    constants.blur = glm::vec4(kSsaoBlurDepthTolerance, 0.f, 0.f, 0.f);
    constants.counts = glm::uvec4(settings.sampleCount, 0u, 0u, 0u);
    // Rebuilt only when the count moves, so a steady frame allocates nothing.
    if (_kernel.size() != settings.sampleCount)
    {
        _kernel = BuildSsaoKernel(settings.sampleCount);
    }
    for (std::size_t i = 0; i < _kernel.size(); ++i)
    {
        constants.kernel[i] = glm::vec4(_kernel[i], 0.f);
    }
    commandList->writeBuffer(_constants, &constants, sizeof(constants));

    {
        ASSISI_PROFILE_GPU_PASS(commandList, "ssao-occlusion");
        RunStep(commandList, Step::Occlusion);
    }
    {
        ASSISI_PROFILE_GPU_PASS(commandList, "ssao-blur");
        RunStep(commandList, Step::BlurHorizontal);
        RunStep(commandList, Step::BlurVertical);
    }
}

void SsaoPass::RunStep(nvrhi::ICommandList *commandList, Step step) const
{
    nvrhi::GraphicsState state;
    BlurAxis axis(0);
    switch (step)
    {
    case Step::Occlusion:
        state.pipeline = _occlusionPipeline;
        state.framebuffer = _occlusionFramebuffer;
        break;
    case Step::BlurHorizontal:
        state.pipeline = _blurPipeline;
        state.framebuffer = _blurScratchFramebuffer;
        axis = BlurAxis(1, 0);
        break;
    case Step::BlurVertical:
    case Step::Count:
        state.pipeline = _blurPipeline;
        state.framebuffer = _occlusionFramebuffer;
        axis = BlurAxis(0, 1);
        break;
    }
    state.addBindingSet(_bindingSets[static_cast<uint32_t>(step)]);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(_width), static_cast<float>(_height)));
    commandList->setGraphicsState(state);
    if (step == Step::BlurHorizontal || step == Step::BlurVertical)
    {
        commandList->setPushConstants(&axis, sizeof(axis));
    }

    nvrhi::DrawArguments draw;
    draw.vertexCount = 3;
    commandList->draw(draw);
}

} // namespace Assisi::Render
