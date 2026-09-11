/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/SceneDistancePass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/GpuMarker.hpp>
#include <Assisi/Render/ShaderModule.hpp>
#include <Assisi/Render/Ssao.hpp>

namespace Assisi::Render
{

namespace
{
// Distance per pixel in metres. Full float: a half would round a distance of
// fifty metres to the nearest few centimetres, which is coarser than the
// tolerances every reader compares against.
constexpr nvrhi::Format kDistanceFormat = nvrhi::Format::R32_FLOAT;

// Mirrors scene_distance.frag's SceneDistanceConstants block.
struct SceneDistanceConstants
{
    /// x = xScale, y = yScale, z = depthScale, w = depthBias (see SsaoProjection).
    glm::vec4 projection;
};

ASSISI_GPU_LAYOUT(SceneDistanceConstants);
ASSISI_GPU_FIRST_FIELD(SceneDistanceConstants, projection);
ASSISI_GPU_NO_TAIL_PADDING(SceneDistanceConstants, projection);
} // namespace

bool SceneDistancePass::Initialize(const InitParams &params)
{
    _device = params.device;

    _vertexShader = LoadSpirvShader(_device, params.vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    const nvrhi::ShaderHandle shader =
        LoadSpirvShader(_device, params.distanceShaderSpvPath, nvrhi::ShaderType::Pixel);
    const nvrhi::ShaderHandle multisampleShader =
        LoadSpirvShader(_device, params.multisampleDistanceShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !shader || !multisampleShader)
    {
        return false;
    }

    nvrhi::BindingLayoutDesc layout;
    layout.visibility = nvrhi::ShaderType::Pixel;
    layout.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    _layout = _device->createBindingLayout(layout);

    nvrhi::FramebufferInfo target;
    target.colorFormats.push_back(kDistanceFormat);
    const auto makePipeline = [this, &target](nvrhi::IShader *pixelShader)
    {
        nvrhi::GraphicsPipelineDesc desc;
        desc.primType = nvrhi::PrimitiveType::TriangleList;
        desc.VS = _vertexShader;
        desc.PS = pixelShader;
        desc.addBindingLayout(_layout);
        desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        desc.renderState.depthStencilState.depthTestEnable = false;
        desc.renderState.depthStencilState.depthWriteEnable = false;
        return _device->createGraphicsPipeline(desc, target);
    };
    const nvrhi::GraphicsPipelineHandle pipeline = makePipeline(shader);
    const nvrhi::GraphicsPipelineHandle multisamplePipeline = makePipeline(multisampleShader);
    if (!pipeline || !multisamplePipeline)
    {
        Core::Log::Error("SceneDistancePass: failed to create the pipelines.");
        return false;
    }

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(SceneDistanceConstants);
    constantsDesc.isConstantBuffer = true;
    constantsDesc.debugName = "SceneDistancePass::Constants";
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    _constants = _device->createBuffer(constantsDesc);
    if (_constants == nullptr)
    {
        Core::Log::Error("SceneDistancePass: failed to create the constant buffer.");
        return false;
    }

    // Last, because IsValid() reads them: a pass whose buffer failed must not
    // report itself usable.
    _pipeline = pipeline;
    _multisamplePipeline = multisamplePipeline;
    return true;
}

bool SceneDistancePass::Configure(std::uint32_t width, std::uint32_t height, nvrhi::ITexture *depth)
{
    if (width == 0 || height == 0 || depth == nullptr)
    {
        Release();
        return false;
    }
    const bool resized = width != _width || height != _height || _distance == nullptr;
    if (!resized && depth == _depth)
    {
        return true;
    }

    _width = width;
    _height = height;
    _depth = depth;
    if (resized)
    {
        nvrhi::TextureDesc desc;
        desc.width = _width;
        desc.height = _height;
        desc.format = kDistanceFormat;
        desc.isRenderTarget = true;
        desc.isShaderResource = true;
        desc.initialState = nvrhi::ResourceStates::ShaderResource;
        desc.keepInitialState = true;
        desc.debugName = "SceneDistancePass::Distance";
        _distance = _device->createTexture(desc);
        _framebuffer = _distance != nullptr
                           ? _device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(_distance))
                           : nullptr;
        if (_framebuffer == nullptr)
        {
            Core::Log::Error("SceneDistancePass: failed to allocate the {}x{} target.", width, height);
            Release();
            return false;
        }
    }

    nvrhi::BindingSetDesc set;
    set.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, _constants));
    set.addItem(nvrhi::BindingSetItem::Texture_SRV(0, _depth));
    _bindingSet = _device->createBindingSet(set, _layout);
    return _bindingSet != nullptr;
}

void SceneDistancePass::Release()
{
    _bindingSet = nullptr;
    _framebuffer = nullptr;
    _distance = nullptr;
    _depth = nullptr;
    _width = 0;
    _height = 0;
}

void SceneDistancePass::Render(nvrhi::ICommandList *commandList, const glm::mat4 &projection) const
{
    const SsaoProjection lens = MakeSsaoProjection(projection);
    const SceneDistanceConstants constants{
        .projection = glm::vec4(lens.xScale, lens.yScale, lens.depthScale, lens.depthBias)};
    commandList->writeBuffer(_constants, &constants, sizeof(constants));

    ASSISI_PROFILE_GPU_PASS(commandList, "scene-distance");
    nvrhi::GraphicsState state;
    state.pipeline = _depth->getDesc().sampleCount > 1 ? _multisamplePipeline : _pipeline;
    state.framebuffer = _framebuffer;
    state.addBindingSet(_bindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(_width), static_cast<float>(_height)));
    commandList->setGraphicsState(state);

    nvrhi::DrawArguments draw;
    draw.vertexCount = 3;
    commandList->draw(draw);
}

} // namespace Assisi::Render
