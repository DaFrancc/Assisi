/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/SkyPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/ShaderModule.hpp>

namespace Assisi::Render
{

bool SkyPass::Initialize(const InitParams &params)
{
    _device = params.device;

    _vertexShader = LoadSpirvShader(_device, params.vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = LoadSpirvShader(_device, params.pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        return false;
    }

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(SkyConstants);
    constantsDesc.isConstantBuffer = true;
    constantsDesc.debugName = "SkyPass::Constants";
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    _constantsBuffer = _device->createBuffer(constantsDesc);
    if (_constantsBuffer == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the constants buffer.");
        return false;
    }

    _moonTexturePath = params.moonTexturePath;

    // White, so a moon drawn before its photograph arrives — or one whose
    // photograph never arrives — is the flat tinted disk rather than a black
    // hole. Allocated here rather than lazily because a binding set has to name
    // a real texture from the first frame, and building one variant of the
    // pipeline for "has a moon" would double something to skip a branch the GPU
    // already skips uniformly.
    _moon.UploadSolidColor(_device, 255, 255, 255, 255, ColorSpace::Srgb, "SkyPass::MoonPlaceholder");
    if (!_moon.IsValid())
    {
        Core::Log::Error("SkyPass: failed to create the moon placeholder texture.");
        return false;
    }

    nvrhi::SamplerDesc samplerDesc;
    // Trilinear: a half-degree disk is about nine pixels across at 1080p, which
    // is mip five or six of a 512 image, and the derivative-selected level is
    // what stops it sparkling as the camera turns.
    samplerDesc.setAllFilters(true);
    // Clamp, because outside the disk the coordinate runs past [0, 1] under a
    // profile already fading to zero. Repeating would drag the opposite limb in.
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _moonSampler = _device->createSampler(samplerDesc);
    if (_moonSampler == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the moon sampler.");
        return false;
    }

    // Both stages read the block: the vertex shader for the inverse
    // view-projection, the pixel shader for everything else.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    _bindingLayout = _device->createBindingLayout(layoutDesc);
    if (_bindingLayout == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky binding layout.");
        return false;
    }

    if (!BuildBindingSet())
    {
        return false;
    }

    return BuildPipeline(params.framebufferInfo);
}

bool SkyPass::BuildBindingSet()
{
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _constantsBuffer));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, _moon.NativeTexture()));
    setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, _moonSampler));
    _bindingSet = _device->createBindingSet(setDesc, _bindingLayout);
    if (_bindingSet == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky binding set.");
        return false;
    }
    return true;
}

void SkyPass::LoadMoonTexture()
{
    _moonState = MoonTexture::Failed;
    if (_moonTexturePath.empty())
    {
        Core::Log::Warn("SkyPass: no moon texture path; the moon is drawn as a flat disk.");
        return;
    }

    Texture loaded;
    if (!loaded.LoadFromAssets(_device, _moonTexturePath, ColorSpace::Srgb).has_value() || !loaded.IsValid())
    {
        Core::Log::Warn("SkyPass: could not load the moon texture '{}'; the moon is drawn as a flat disk.",
                        _moonTexturePath);
        return;
    }

    _moon = std::move(loaded);
    // The set names the texture, so replacing the texture means replacing the
    // set. The layout is unchanged, so the pipeline is not rebuilt.
    if (!BuildBindingSet())
    {
        return;
    }
    _moonState = MoonTexture::Loaded;
}

bool SkyPass::BuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS = _vertexShader;
    pipelineDesc.PS = _pixelShader;
    pipelineDesc.addBindingLayout(_bindingLayout);

    // No input layout and no vertex buffer: the triangle comes from
    // gl_VertexIndex. Culling is off because that triangle's winding is a
    // property of the index arithmetic rather than of anything a caller controls.
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;

    // Equal against the 1.0 the depth clear left. The scene tests Less and so
    // wrote something smaller wherever it drew, which is what makes covered
    // pixels fail here and cost nothing. Depth is not written, so the scene's
    // depth reaches the passes after this one intact.
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
    pipelineDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Equal;

    _pipeline = _device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (_pipeline == nullptr)
    {
        Core::Log::Error("SkyPass: failed to create the sky pipeline.");
        return false;
    }
    return true;
}

bool SkyPass::RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    if (_bindingLayout == nullptr)
    {
        return true; // nothing built yet — nothing to rebuild
    }
    return BuildPipeline(framebufferInfo);
}

void SkyPass::Draw(const RenderFrame &frame, const glm::mat4 &viewProjection, const glm::vec3 &cameraPosition,
                   const SkySun &sun, const SkyMoon &moon, const SkySettings &settings)
{
    if (!IsValid())
    {
        return;
    }

    ASSISI_PROFILE_GPU_PASS(frame.commandList, "sky");

    // Pay for what you place: the file is opened on the first frame something
    // asks for a moon disk, and never at all in a level without one.
    if (moon.diskIntensity > 0.0f && _moonState == MoonTexture::NotLoaded)
    {
        LoadMoonTexture();
    }

    const SkyConstants constants = MakeSkyConstants(glm::inverse(viewProjection), cameraPosition, sun, moon, settings);
    Submit(frame.commandList, frame.framebuffer, _pipeline,
           nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)), constants);
}

void SkyPass::DrawInto(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *target, const SkyConstants &constants)
{
    if (!IsValid() || target == nullptr)
    {
        return;
    }

    if (_capturePipeline == nullptr)
    {
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.VS = _vertexShader;
        pipelineDesc.PS = _pixelShader;
        pipelineDesc.addBindingLayout(_bindingLayout);
        pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        // No depth to test against: every texel of a capture target is sky.
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
        _capturePipeline = _device->createGraphicsPipeline(pipelineDesc, target->getFramebufferInfo());
        if (_capturePipeline == nullptr)
        {
            Core::Log::Error("SkyPass: failed to create the sky capture pipeline.");
            return;
        }
    }
    const nvrhi::FramebufferInfoEx &info = target->getFramebufferInfo();
    Submit(commandList, target, _capturePipeline,
           nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)), constants);
}

void SkyPass::Submit(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *target,
                     nvrhi::IGraphicsPipeline *pipeline, const nvrhi::Viewport &viewport,
                     const SkyConstants &constants)
{
    commandList->writeBuffer(_constantsBuffer, &constants, sizeof(constants));

    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = target;
    state.addBindingSet(_bindingSet);
    state.viewport.addViewportAndScissorRect(viewport);
    commandList->setGraphicsState(state);

    // One triangle covering the target; sky.vert makes it from the index.
    constexpr uint32_t kFullscreenTriangleVertices = 3;
    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = kFullscreenTriangleVertices;
    commandList->draw(drawArgs);
}

} // namespace Assisi::Render
