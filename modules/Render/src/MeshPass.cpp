/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshPass.hpp>

#include <Assisi/Core/Logger.hpp>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <vector>

namespace Assisi::Render
{

namespace
{
/// @brief Reads a compiled SPIR-V shader relative to the working directory.
///
/// Not routed through AssetSystem: the build places compiled .spv output next
/// to the executable (see apps/sandbox/CMakeLists.txt), not under assets/. See
/// docs/nvrhi-migration-todo.md section 4 for the pending decision on unifying
/// the shader pipeline.
std::vector<char> ReadSpirvFile(const std::string &path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        Assisi::Core::Log::Error("MeshPass: failed to open shader file: {}", path);
        return {};
    }

    const size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

nvrhi::ShaderHandle LoadShader(nvrhi::IDevice *device, const std::string &path, nvrhi::ShaderType stage)
{
    const std::vector<char> spirv = ReadSpirvFile(path);
    if (spirv.empty())
    {
        return nullptr;
    }

    nvrhi::ShaderDesc desc;
    desc.shaderType = stage;
    desc.debugName = path;
    return device->createShader(desc, spirv.data(), spirv.size());
}
} // namespace

bool MeshPass::Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &framebufferInfo)
{
    const nvrhi::ShaderHandle vertexShader = LoadShader(device, "shaders/cube_min.vert.spv", nvrhi::ShaderType::Vertex);
    const nvrhi::ShaderHandle fragmentShader = LoadShader(device, "shaders/cube_min.frag.spv", nvrhi::ShaderType::Pixel);
    if (!vertexShader || !fragmentShader)
    {
        return false;
    }

    const nvrhi::VertexAttributeDesc attributes[] = {
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(offsetof(Vertex, Position))
            .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
            .setName("NORMAL")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(offsetof(Vertex, Normal))
            .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
            .setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(offsetof(Vertex, TextureCoordinates))
            .setElementStride(sizeof(Vertex)),
        nvrhi::VertexAttributeDesc()
            .setName("TANGENT")
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setOffset(offsetof(Vertex, Tangent))
            .setElementStride(sizeof(Vertex)),
    };
    _inputLayout = device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), vertexShader);

    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex;
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::mat4)));
    _bindingLayout = device->createBindingLayout(bindingLayoutDesc);

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::mat4)));
    _bindingSet = device->createBindingSet(bindingSetDesc, _bindingLayout);

    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.inputLayout = _inputLayout;
    pipelineDesc.VS = vertexShader;
    pipelineDesc.PS = fragmentShader;
    pipelineDesc.addBindingLayout(_bindingLayout);
    pipelineDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;

    _pipeline = device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
    return _pipeline != nullptr;
}

void MeshPass::Draw(nvrhi::ICommandList *commandList, nvrhi::IFramebuffer *framebuffer, uint32_t viewportWidth,
                     uint32_t viewportHeight, const glm::mat4 &modelViewProjection, const MeshBuffer &mesh) const
{
    nvrhi::GraphicsState state;
    state.pipeline = _pipeline;
    state.framebuffer = framebuffer;
    state.addBindingSet(_bindingSet);
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
    state.addVertexBuffer(nvrhi::VertexBufferBinding{mesh.VertexBuffer(), 0, 0});
    state.indexBuffer = nvrhi::IndexBufferBinding{mesh.IndexBuffer(), nvrhi::Format::R32_UINT, 0};
    commandList->setGraphicsState(state);

    commandList->setPushConstants(&modelViewProjection, sizeof(modelViewProjection));

    nvrhi::DrawArguments drawArgs;
    drawArgs.vertexCount = mesh.IndexCount();
    commandList->drawIndexed(drawArgs);
}

} // namespace Assisi::Render
