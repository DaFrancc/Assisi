/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Engine/QuadPass.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/ShaderModule.hpp>

#include <array>
#include <cstdint>

namespace Assisi::Mondrian::Engine
{
namespace
{

constexpr const char *kVertexShaderPath = "shaders/mondrian_quad.vert.spv";
constexpr const char *kPixelShaderPath  = "shaders/mondrian_quad.frag.spv";

/// Two triangles, generated in the vertex shader from the vertex index.
constexpr uint32_t kVerticesPerQuad = 6;

/// One quad's worth of state, mirroring the push-constant block in
/// mondrian_quad.vert and mondrian_quad.frag member for member.
struct QuadConstants
{
    std::array<float, 4> rect;     ///< x, y, width, height in pixels
    std::array<float, 4> color;    ///< straight-alpha display colour
    std::array<float, 2> viewport; ///< target width and height in pixels
};
// The shaders read this block at fixed offsets; a member added or reordered on
// one side alone would draw every quad from the wrong numbers.
static_assert(sizeof(QuadConstants) == 40);

} // namespace

bool QuadPass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo)
{
    _vertexShader = Render::LoadSpirvShader(device, kVertexShaderPath, nvrhi::ShaderType::Vertex);
    _pixelShader  = Render::LoadSpirvShader(device, kPixelShaderPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShader)
    {
        Core::Log::Error("Mondrian: the quad shaders could not be loaded.");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(QuadConstants)));
    _bindingLayout = device->createBindingLayout(layoutDesc);

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(QuadConstants)));
    _bindingSet = _bindingLayout ? device->createBindingSet(setDesc, _bindingLayout) : nullptr;
    if (!_bindingSet)
    {
        Core::Log::Error("Mondrian: the quad binding set could not be created.");
        return false;
    }

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.VS       = _vertexShader;
    pipelineDesc.PS       = _pixelShader;
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    // The UI sits over everything the scene drew, whatever depth it left.
    pipelineDesc.renderState.depthStencilState.depthTestEnable  = false;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = false;
    // Premultiplied: the fragment shader has already scaled colour by alpha.
    pipelineDesc.renderState.blendState.targets[0]
        .setBlendEnable(true)
        .setSrcBlend(nvrhi::BlendFactor::One)
        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

    _pipeline = device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    if (!_pipeline)
    {
        Core::Log::Error("Mondrian: the quad pipeline could not be created.");
        return false;
    }
    return true;
}

void QuadPass::Draw(const Render::RenderFrame &frame, const DrawList &drawList)
{
    if (!_pipeline || drawList.Quads().empty())
    {
        return;
    }

    nvrhi::GraphicsState state;
    state.pipeline    = _pipeline;
    state.framebuffer = frame.framebuffer;
    state.addBindingSet(_bindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    frame.commandList->setGraphicsState(state);

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = kVerticesPerQuad;
    for (const DrawQuad &quad : drawList.Quads())
    {
        const QuadConstants constants{
            .rect     = {quad.rect.x, quad.rect.y, quad.rect.width, quad.rect.height},
            .color    = {quad.color.r, quad.color.g, quad.color.b, quad.color.a},
            .viewport = {static_cast<float>(frame.width), static_cast<float>(frame.height)}};
        frame.commandList->setPushConstants(&constants, sizeof(constants));
        frame.commandList->draw(drawArgs);
    }
}

void QuadPass::Shutdown()
{
    _pipeline      = nullptr;
    _bindingSet    = nullptr;
    _bindingLayout = nullptr;
    _pixelShader   = nullptr;
    _vertexShader  = nullptr;
}

} // namespace Assisi::Mondrian::Engine
