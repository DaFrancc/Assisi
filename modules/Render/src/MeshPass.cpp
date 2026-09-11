/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/MeshPass.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/GpuLayout.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/ShaderModule.hpp>
#include <Assisi/Render/ShadowView.hpp>
#include <Assisi/Render/SpecularIbl.hpp>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <iterator>
#include <utility>
#include <vector>

namespace Assisi::Render
{

namespace
{
// Per-instance data: one record per drawn submesh, uploaded to the instance
// buffer each frame and indexed in the vertex shader by gl_InstanceIndex (each
// draw sets startInstanceLocation to its record). Mirrors mesh.vert's
// `InstanceData` std430 struct — mat4 (0..63) + uint (64), padded to the 16-byte
// array stride std430 gives the struct.
// (Declared as MeshPass::InstanceData in the header so the pass can hold a
// reusable scratch vector of them across frames.)
using InstanceData = MeshPass::InstanceData;

// Where each of the pass's resources sits in its register space. Each must match
// the layout(binding = …) mesh.frag and mesh.vert declare it at; the backend
// offsets samplers and constant buffers into their own ranges, so the same
// number in two of these is two different bindings.
enum class ResourceSlot : uint32_t
{
    Materials = 0,
    PointLights = 1,
    SpotLights = 2,
    DirLights = 3,
    LightIndices = 4,
    LightGrids = 5,
    Instances = 6,
    ShadowCascades = 7,
    ShadowViews = 8,
    ShadowAtlas = 9,
    Environment = 10,
    BrdfTable = 11,
    ScreenOcclusion = 12,
};

enum class SamplerSlot : uint32_t
{
    Material = 0,
    Shadow = 1,
    Clamp = 2,
};

enum class ConstantBufferSlot : uint32_t
{
    Frame = 0,
};

// Starting instance-buffer capacity (records). Grows geometrically past this when
// a frame draws more items; the first level typically fits without a growth. Also
// the starting capacity for the indirect-args buffer (batches <= instances).
constexpr uint32_t kInitialInstanceCapacity = 1024u;

// The indirect command matches Vulkan's VkDrawIndexedIndirectCommand byte-for-byte
// (five tightly-packed 32-bit fields), so the batch vector uploads straight into
// the indirect-args buffer with no repack.
static_assert(sizeof(nvrhi::DrawIndexedIndirectArguments) == 20,
              "DrawIndexedIndirectArguments must match VkDrawIndexedIndirectCommand's packed layout.");

// Per-frame data (view-projection + camera + cluster-grid parameters), uploaded
// once per frame via UpdateFrameConstants() into a constant buffer. viewProjection
// leads so the vertex shader can form clip position from each instance's world
// matrix. Mirrors mesh.vert/frag's matching `uniform FrameConstants` block.
struct FrameConstants
{
    glm::mat4 viewProjection;
    glm::mat4 view;
    glm::uvec4 gridDim;           // xyz used, w unused
    glm::vec4 screenSizeNearFar;  // xy = screen size, z = nearZ, w = farZ
    glm::uvec4 lightCounts;       // x = directional light count, y = debug view, zw unused
    /// World-space camera position, w unused. Derived once here rather than per
    /// fragment: it is constant across the frame, but view is a uniform, so the
    /// shader compiler cannot hoist the -transpose(mat3(view)) * view[3] out.
    glm::vec4 cameraPosition;
    /// Froxel lookup scale/bias, so mesh.frag's ClusterIndex() is FMAs and a
    /// single log instead of three divides and two logs (the Doom-2016 form):
    ///   xy = gridDim.xy / screenSize        (screen pixel -> cluster column/row)
    ///   z  = gridDim.z / log(farZ / nearZ)  (log-depth slice scale)
    ///   w  = -z * log(nearZ)                (matching bias)
    /// slice = log(|viewZ|) * z + w, which is gridDim.z * log(|viewZ|/nearZ) / log(farZ/nearZ).
    glm::vec4 clusterScale;
    /// The frame's indirect term, as its provider answered it (see
    /// Render::IndirectConstants): rgb = the radiance a surface facing straight
    /// up receives, then the same facing straight down, w unused in both.
    glm::vec4 indirectSky;
    glm::vec4 indirectGround;
    /// x = 1 while a prefiltered environment answers the specular half, y =
    /// its last mip, z = 1 while screen-space occlusion ran this frame, w unused.
    glm::vec4 indirectSpecular;

    /// x = cascade count (0 = nothing shadows this frame, and the shader takes
    /// no lookup at all), y = which directional light the cascades belong to,
    /// z = ShadowFilter, w = 1 to tint by cascade.
    glm::uvec4 shadowCounts;
    /// x = the ShadowFilter the atlas is sampled with, y = 1 while any local
    /// light holds a tile and the two light loops should look one up, z = 1
    /// while those lookups take the contact-hardening path, w = the atlas
    /// format's depth steps (see ShadowMapDepthSteps).
    ///
    /// The biases and the tap step are deliberately absent: a demoted tile is
    /// biased for the smaller map it got, so those belong per view rather than
    /// per frame, and they ride in the shadow view table with the matrix.
    glm::uvec4 localShadowCounts;
    /// x = one texel of the map, in UV, which is the step between PCF taps in
    /// every cascade whose texels are small enough,
    /// y = the fraction of each cascade spent fading into the next,
    /// z = the penumbra cap over the filter's radius, which the shader divides
    /// by a cascade's depth range to get the widest step that cascade may use,
    /// w = the sun's penumbra per world unit of blocker distance over the same
    /// radius, which the shader multiplies by the distance it reads out of the
    /// map to get the step the scene actually calls for. The cascade's depth
    /// range cancels in that product, which is why no cascade term appears.
    glm::vec4 shadowParams;
    /// The sun's contact-hardening constants (see SunPcssConstants): x = its
    /// penumbra UV per unit of depth, y = the reach cap in UV, z = one texel of
    /// depth, w = kMaxPenumbraWorld. All zero while the sun takes the fixed
    /// kernel, and x is what the shader tests: the sun's own figure is never
    /// zero, so zero can only mean off. A local light's ride in its view instead.
    ///
    /// The world cap rides here rather than being read off shadowParams.z,
    /// which is quoted over the selected filter's radius; this path's kernel is
    /// always the Vogel disk.
    glm::vec4 shadowPcss;
    /// One record per cascade: x = the view-space distance it ends at (what the
    /// shader selects on), y = its constant depth bias already in the [0, 1]
    /// depth the shader compares in, z = its normal offset in world units,
    /// w = the world span of its depth range, which is also how wide its ortho
    /// box is, and what the tap-step cap above is divided by to reach a step.
    /// Both biases are scaled CPU-side by that cascade's texel size,
    /// which is why one setting holds across cascades whose texels differ by an
    /// order of magnitude (see CascadeDepthBiasNdc).
    ///
    /// One array of records rather than three arrays of scalars: std140 pads a
    /// float array to a 16-byte stride anyway, so three of them would cost the
    /// same and read as three places to keep in step instead of one. Entries
    /// past the live count are unread.
    std::array<glm::vec4, kMaxShadowCascades> shadowCascade;
    /// World space to each cascade's clip space. Last because it is the only
    /// member whose size is not one lane, and appending keeps every offset
    /// above it fixed.
    std::array<glm::mat4, kMaxShadowCascades> shadowViewProjection;
};

// std140, not std430 — this is a uniform block. The two agree on everything this
// struct contains, because every member is a vec4/uvec4/mat4 lane or an array of
// one, and those take a 16-byte offset and stride under both rules. That is not
// an accident to be preserved by luck: it is why nothing here is a bare float or
// a uint, and the offsets below are what keeps it true.
//
// GLM's default gentypes are 4-aligned, so the C++ side packs these tightly and
// every member still lands on a lane boundary because every member is a whole
// number of lanes wide. Insert anything narrower and the two layouts part
// company silently — which is what these lines exist to prevent.
ASSISI_GPU_LAYOUT(FrameConstants);
ASSISI_GPU_FIRST_FIELD(FrameConstants, viewProjection);
ASSISI_GPU_FIELD_AFTER(FrameConstants, view, viewProjection);
ASSISI_GPU_FIELD_AFTER(FrameConstants, gridDim, view);
ASSISI_GPU_FIELD_AFTER(FrameConstants, screenSizeNearFar, gridDim);
ASSISI_GPU_FIELD_AFTER(FrameConstants, lightCounts, screenSizeNearFar);
ASSISI_GPU_FIELD_AFTER(FrameConstants, cameraPosition, lightCounts);
ASSISI_GPU_FIELD_AFTER(FrameConstants, clusterScale, cameraPosition);
ASSISI_GPU_FIELD_AFTER(FrameConstants, indirectSky, clusterScale);
ASSISI_GPU_FIELD_AFTER(FrameConstants, indirectGround, indirectSky);
ASSISI_GPU_FIELD_AFTER(FrameConstants, indirectSpecular, indirectGround);
ASSISI_GPU_FIELD_AFTER(FrameConstants, shadowCounts, indirectSpecular);
ASSISI_GPU_FIELD_AFTER(FrameConstants, localShadowCounts, shadowCounts);
ASSISI_GPU_FIELD_AFTER(FrameConstants, shadowParams, localShadowCounts);
ASSISI_GPU_FIELD_AFTER(FrameConstants, shadowPcss, shadowParams);
ASSISI_GPU_FIELD_AFTER(FrameConstants, shadowCascade, shadowPcss);
ASSISI_GPU_FIELD_AFTER(FrameConstants, shadowViewProjection, shadowCascade);
ASSISI_GPU_NO_TAIL_PADDING(FrameConstants, shadowViewProjection);
} // namespace

bool MeshPass::Initialize(const InitParams &params)
{
    nvrhi::IDevice *const device = params.device;
    const nvrhi::FramebufferInfo &framebufferInfo = params.framebufferInfo;
    const std::string &vertexShaderSpvPath = params.vertexShaderSpvPath;

    _device = device;
    _clusterGrid = params.clusterGrid;
    _bindlessLayout = params.bindlessLayout;
    _bindlessTable = params.bindlessTable;
    _materialTable = params.materialTable;
    _depthVertexShaderPath = params.depthVertexShaderSpvPath;
    _maskedDepthVertexShaderPath = params.maskedDepthVertexShaderSpvPath;
    _maskedDepthPixelShaderPath = params.maskedDepthPixelShaderSpvPath;
    _invariantVertexShaderPath = params.invariantVertexShaderSpvPath;

    _vertexShader = LoadSpirvShader(device, vertexShaderSpvPath, nvrhi::ShaderType::Vertex);
    _pixelShaders[kPixelShaderOpaque] = LoadSpirvShader(device, params.pixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    // The masked build is not optional: without it a cutout material would draw
    // solid, which is a wrong image rather than a missing feature.
    _pixelShaders[kPixelShaderMasked] =
        LoadSpirvShader(device, params.maskedPixelShaderSpvPath, nvrhi::ShaderType::Pixel);
    if (!_vertexShader || !_pixelShaders[kPixelShaderOpaque] || !_pixelShaders[kPixelShaderMasked])
    {
        return false;
    }

    using Assisi::Geometry::Vertex;
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
    _inputLayout = device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), _vertexShader);

    // Set 0 — the one binding set every draw shares (stage D). Texture_SRV/Sampler
    // are separate descriptors in NVRHI's Vulkan backend (HLSL t/s split, not
    // GLSL's combined sampler2D); StructuredBuffer_SRV shares the t-register space
    // with Texture_SRV. The slots are ResourceSlot, SamplerSlot and
    // ConstantBufferSlot. No push constants: per-object data (world matrix +
    // material id) is read from the instance buffer by gl_InstanceIndex, and the
    // material's textures from the bindless table (register space 1).
    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
    using nvrhi::BindingLayoutItem;
    bindingLayoutDesc.addItem(BindingLayoutItem::ConstantBuffer(std::to_underlying(ConstantBufferSlot::Frame)));
    bindingLayoutDesc.addItem(BindingLayoutItem::Sampler(std::to_underlying(SamplerSlot::Material)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::Materials)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::PointLights)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::SpotLights)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::DirLights)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::LightIndices)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::LightGrids)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::Instances)));
    bindingLayoutDesc.addItem(BindingLayoutItem::Texture_SRV(std::to_underlying(ResourceSlot::ShadowCascades)));
    bindingLayoutDesc.addItem(BindingLayoutItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::ShadowViews)));
    bindingLayoutDesc.addItem(BindingLayoutItem::Texture_SRV(std::to_underlying(ResourceSlot::ShadowAtlas)));
    // One comparison sampler for both maps: the cascades and the atlas are
    // filtered identically, and only the rectangle they are read from differs.
    bindingLayoutDesc.addItem(BindingLayoutItem::Sampler(std::to_underlying(SamplerSlot::Shadow)));
    bindingLayoutDesc.addItem(BindingLayoutItem::Texture_SRV(std::to_underlying(ResourceSlot::Environment)));
    bindingLayoutDesc.addItem(BindingLayoutItem::Texture_SRV(std::to_underlying(ResourceSlot::BrdfTable)));
    bindingLayoutDesc.addItem(BindingLayoutItem::Sampler(std::to_underlying(SamplerSlot::Clamp)));
    bindingLayoutDesc.addItem(BindingLayoutItem::Texture_SRV(std::to_underlying(ResourceSlot::ScreenOcclusion)));
    _bindingLayout = device->createBindingLayout(bindingLayoutDesc);

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true); // trilinear: linear min/mag + linear mip blending
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Repeat);
    // Anisotropic filtering keeps textures sharp at grazing angles, where plain
    // trilinear picks an over-coarse mip and washes the surface out. The context
    // reports 1.0 (isotropic) when the device didn't enable the feature, so this
    // is a no-op there rather than an invalid request.
    if (const Vulkan::VulkanContext *context = RenderSystem::GetVulkanContext())
    {
        samplerDesc.maxAnisotropy = context->GetMaxAnisotropy();
    }
    _sampler = device->createSampler(samplerDesc);

    // The cascades are compared, not read: the hardware tests the reference
    // depth against four texels and returns the blended result, so one fetch is
    // already a 2x2 PCF and the kernel sizes below it are that many again.
    // Clamped, because a lookup outside a cascade must not wrap to the far side
    // of the map — the shader rejects those before they get here, and the clamp
    // is what keeps a rounding error at the border from shadowing anything.
    nvrhi::SamplerDesc shadowSamplerDesc;
    shadowSamplerDesc.setAllFilters(true);
    shadowSamplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::ClampToBorder);
    shadowSamplerDesc.setBorderColor(nvrhi::Color(1.f));
    shadowSamplerDesc.setReductionType(nvrhi::SamplerReductionType::Comparison);
    _shadowSampler = device->createSampler(shadowSamplerDesc);
    if (_shadowSampler == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the shadow comparison sampler.");
        return false;
    }

    // Trilinear, because the environment's mips are its roughness and reading
    // between two of them is how a roughness between two is answered. Clamped
    // for the BRDF table, whose edges are n.v = 0 and roughness 1 and must not
    // wrap to the opposite ones; a cube ignores the address mode.
    nvrhi::SamplerDesc clampSamplerDesc;
    clampSamplerDesc.setAllFilters(true);
    clampSamplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    _clampSampler = device->createSampler(clampSamplerDesc);
    if (_clampSampler == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the clamped sampler.");
        return false;
    }

    if (!CreateBrdfTable())
    {
        return false;
    }

    // Bound in the environment's place while no provider has one, which is
    // every frame of a scene with no sky. Never sampled — the shader branches
    // on the frame constant first — and never written; it exists because a
    // binding set may not have a hole in it.
    nvrhi::TextureDesc noCubeDesc;
    noCubeDesc.width = 1;
    noCubeDesc.height = 1;
    noCubeDesc.arraySize = kCubeFaceCount;
    noCubeDesc.dimension = nvrhi::TextureDimension::TextureCube;
    noCubeDesc.format = nvrhi::Format::RGBA16_FLOAT;
    noCubeDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    noCubeDesc.keepInitialState = true;
    noCubeDesc.debugName = "MeshPass::NoEnvironment";
    _noEnvironmentCube = device->createTexture(noCubeDesc);
    if (_noEnvironmentCube == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the empty environment cube.");
        return false;
    }

    // Bound in screen-space occlusion's place on every frame it does not run,
    // for the same reason and on the same terms: never sampled, never written.
    nvrhi::TextureDesc noOcclusionDesc;
    noOcclusionDesc.width = 1;
    noOcclusionDesc.height = 1;
    noOcclusionDesc.format = nvrhi::Format::R8_UNORM;
    noOcclusionDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    noOcclusionDesc.keepInitialState = true;
    noOcclusionDesc.debugName = "MeshPass::NoAmbientOcclusion";
    _noAmbientOcclusion = device->createTexture(noOcclusionDesc);
    if (_noAmbientOcclusion == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the empty occlusion texture.");
        return false;
    }

    nvrhi::BufferDesc frameConstantsDesc;
    frameConstantsDesc.byteSize = sizeof(FrameConstants);
    frameConstantsDesc.isConstantBuffer = true;
    frameConstantsDesc.debugName = "MeshPass::FrameConstants";
    frameConstantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    frameConstantsDesc.keepInitialState = true;
    _frameConstantsBuffer = device->createBuffer(frameConstantsDesc);
    if (_frameConstantsBuffer == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the frame-constants buffer.");
        return false;
    }

    // One row, never read. It is bound in the view table's place while there is
    // no table, which for a scene that shadows nothing is every frame: a binding
    // set may not have a hole in it, and a scene with no shadows must still draw.
    _noShadowViews.Create(device, sizeof(ShadowViewGpu), 1u, /*allowUnorderedAccess=*/ false,
                          "MeshPass::NoShadowViews");
    if (_noShadowViews.NativeBuffer() == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the empty shadow view table.");
        return false;
    }

    return RebuildPipeline(framebufferInfo);
}

bool MeshPass::RebuildPipeline(const nvrhi::FramebufferInfo &framebufferInfo)
{
    // The prepass's sets were built against the old framebuffer, and the next
    // PreparePrepass rebuilds them against this one.
    _framebufferInfo = framebufferInfo;
    for (uint32_t i = 0; i < kMeshPipelineCount; ++i)
    {
        _prepassPipelines[i] = nullptr;
        _afterPrepassPipelines[i] = nullptr;
    }

    // The pipelines differ only in their pixel shader and their cull mode. Building
    // them from one desc is what keeps everything else identical: a cutout surface,
    // or the inside of one, must shade and depth-test exactly as the opaque front
    // face does, or they would diverge in more than the property that separates them.
    nvrhi::GraphicsPipelineDesc pipelineDesc;
    pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
    pipelineDesc.inputLayout = _inputLayout;
    pipelineDesc.VS = _vertexShader;
    pipelineDesc.addBindingLayout(_bindingLayout);  // set 0: CBs, sampler, light buffers
    pipelineDesc.addBindingLayout(_bindlessLayout);  // set 1: bindless material textures
    // Meshes are authored CCW-front (standard convention). NVRHI's Vulkan backend
    // flips the viewport (VKViewportWithDXCoords) to undo Vulkan's native Y-down
    // clip space, which also flips the winding order the rasterizer perceives —
    // without this, back-face culling culls the actual front faces instead.
    pipelineDesc.renderState.rasterState.frontCounterClockwise = true;
    pipelineDesc.renderState.depthStencilState.depthTestEnable = true;
    pipelineDesc.renderState.depthStencilState.depthWriteEnable = true;

    for (uint32_t i = 0; i < kMeshPipelineCount; ++i)
    {
        const MeshPipeline pipeline = static_cast<MeshPipeline>(i);
        pipelineDesc.PS = _pixelShaders[MeshPipelineIsMasked(pipeline) ? kPixelShaderMasked : kPixelShaderOpaque];
        pipelineDesc.renderState.rasterState.cullMode =
            MeshPipelineIsDoubleSided(pipeline) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
        _pipelines[i] = _device->createGraphicsPipeline(pipelineDesc, framebufferInfo);
        if (_pipelines[i] == nullptr)
        {
            Core::Log::Error("MeshPass: failed to create the graphics pipeline.");
            return false;
        }
    }
    return true;
}

bool MeshPass::PreparePrepass()
{
    if (_prepassPipelines[0] != nullptr && _afterPrepassPipelines[0] != nullptr)
    {
        return true;
    }

    const auto load = [this](nvrhi::ShaderHandle &shader, const std::string &path, nvrhi::ShaderType type)
    {
        if (shader == nullptr)
        {
            shader = LoadSpirvShader(_device, path, type);
        }
        return shader != nullptr;
    };
    if (!load(_depthVertexShader, _depthVertexShaderPath, nvrhi::ShaderType::Vertex) ||
        !load(_maskedDepthVertexShader, _maskedDepthVertexShaderPath, nvrhi::ShaderType::Vertex) ||
        !load(_maskedDepthPixelShader, _maskedDepthPixelShaderPath, nvrhi::ShaderType::Pixel) ||
        !load(_invariantVertexShader, _invariantVertexShaderPath, nvrhi::ShaderType::Vertex))
    {
        return false;
    }

    // The same desc the lit set is built from, so the three differ in nothing
    // but what is set below: a surface that rasterized differently in the
    // prepass would hold depth the lit pass cannot match.
    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.inputLayout = _inputLayout;
    desc.addBindingLayout(_bindingLayout);
    desc.addBindingLayout(_bindlessLayout);
    desc.renderState.rasterState.frontCounterClockwise = true;
    desc.renderState.depthStencilState.depthTestEnable = true;

    for (uint32_t i = 0; i < kMeshPipelineCount; ++i)
    {
        const MeshPipeline pipeline = static_cast<MeshPipeline>(i);
        const bool masked = MeshPipelineIsMasked(pipeline);
        desc.renderState.rasterState.cullMode =
            MeshPipelineIsDoubleSided(pipeline) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;

        // Depth and nothing else. The scene framebuffer has a colour target,
        // and a pipeline with no fragment stage leaves what it would write
        // there undefined, so the mask is what keeps the clear intact for the
        // sky. An opaque surface gets no fragment stage at all, which is what
        // keeps early depth rejection whole.
        desc.VS = masked ? _maskedDepthVertexShader : _depthVertexShader;
        desc.PS = masked ? _maskedDepthPixelShader : nullptr;
        desc.renderState.depthStencilState.depthWriteEnable = true;
        desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Less;
        desc.renderState.blendState.targets[0].colorWriteMask = static_cast<nvrhi::ColorMask>(0);
        _prepassPipelines[i] = _device->createGraphicsPipeline(desc, _framebufferInfo);

        // Shades each pixel once, for the surface the prepass kept there: equal
        // passes it and everything behind fails. The depth is already written,
        // and writing it again would only cost bandwidth.
        desc.VS = _invariantVertexShader;
        desc.PS = _pixelShaders[masked ? kPixelShaderMasked : kPixelShaderOpaque];
        desc.renderState.depthStencilState.depthWriteEnable = false;
        desc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        desc.renderState.blendState.targets[0].colorWriteMask = nvrhi::ColorMask::All;
        _afterPrepassPipelines[i] = _device->createGraphicsPipeline(desc, _framebufferInfo);

        if (_prepassPipelines[i] == nullptr || _afterPrepassPipelines[i] == nullptr)
        {
            Core::Log::Error("MeshPass: failed to create the depth prepass pipelines.");
            for (uint32_t j = 0; j < kMeshPipelineCount; ++j)
            {
                _prepassPipelines[j] = nullptr;
                _afterPrepassPipelines[j] = nullptr;
            }
            return false;
        }
    }
    return true;
}

const nvrhi::GraphicsPipelineHandle *MeshPass::PipelinesFor(MeshPassStage stage) const
{
    switch (stage)
    {
    case MeshPassStage::DepthPrepass:    return _prepassPipelines;
    case MeshPassStage::LitAfterPrepass: return _afterPrepassPipelines;
    case MeshPassStage::Lit:
    case MeshPassStage::Count:           break;
    }
    return _pipelines;
}

void MeshPass::UpdateFrameConstants(nvrhi::ICommandList *commandList, const FrameConstantsParams &params) const
{
    // A new frame has submitted nothing yet. Without this, a frame whose draw
    // path found nothing to submit would leave Redraw replaying the last one's.
    _lastSubmission = Submission::None;

    FrameConstants constants;
    constants.viewProjection = params.viewProjection;
    constants.view = params.view;
    constants.gridDim = glm::uvec4(ClusterGrid::kNumX, ClusterGrid::kNumY, ClusterGrid::kNumZ, 0u);
    constants.screenSizeNearFar = glm::vec4(static_cast<float>(params.screenWidth),
                                            static_cast<float>(params.screenHeight), params.nearZ, params.farZ);
    constants.lightCounts =
        glm::uvec4(params.dirLightCount, static_cast<uint32_t>(params.debugView), 0u, 0u);

    // View is a rigid transform (View = R | t, t = -R * cameraPos), so the camera
    // position is -R^-1 * t, and R^-1 == transpose(R) because R is orthonormal.
    constants.cameraPosition =
        glm::vec4(-glm::transpose(glm::mat3(params.view)) * glm::vec3(params.view[3]), 0.f);

    // Guard the logs: a zero/negative near or far plane would make these inf/NaN
    // and poison every cluster lookup. Both come from the camera, which validates
    // them, so this is belt-and-braces rather than an expected path.
    const float safeNear = params.nearZ > 0.f ? params.nearZ : 0.001f;
    const float safeFar  = params.farZ > safeNear ? params.farZ : safeNear * 2.f;
    const float logRatio = std::log(safeFar / safeNear);
    const float sliceScale = static_cast<float>(ClusterGrid::kNumZ) / logRatio;
    constants.clusterScale =
        glm::vec4(static_cast<float>(ClusterGrid::kNumX) / static_cast<float>(params.screenWidth),
                  static_cast<float>(ClusterGrid::kNumY) / static_cast<float>(params.screenHeight), sliceScale,
                  -sliceScale * std::log(safeNear));

    constants.indirectSky = glm::vec4(params.indirect.skyRadiance, 0.f);
    constants.indirectGround = glm::vec4(params.indirect.groundRadiance, 0.f);
    // z is not the provider's: occlusion is visibility, applied to whatever
    // radiance the provider answered, and rides in this lane only because the
    // lane belongs to the indirect term it darkens.
    constants.indirectSpecular = glm::vec4(params.indirect.specularEnvironment, params.indirect.specularMaxLod,
                                           params.screenOcclusion ? 1.f : 0.f, 0.f);

    // Shadows. A zero cascade count is the whole of "nothing shadows this frame"
    // as far as the shader is concerned: it takes no lookup, so an unshadowed
    // scene pays one comparison against a constant.
    const ShadowFrameData &shadows = params.shadows;
    const uint32_t cascadeCount = shadows.fit != nullptr ? shadows.fit->count : 0u;
    constants.shadowCounts = glm::uvec4(cascadeCount, shadows.sunLightIndex,
                                        static_cast<uint32_t>(shadows.settings.filter),
                                        static_cast<uint32_t>(shadows.debugView));
    // The local half switches on its own flag rather than on the cascade count:
    // a scene may have shadowed lamps and no sun, or a sun and no shadowed lamp,
    // and neither should pay for the other's lookup.
    constants.localShadowCounts =
        glm::uvec4(static_cast<uint32_t>(shadows.localSettings.filter), shadows.localActive ? 1u : 0u,
                   shadows.localPcss ? 1u : 0u, ShadowMapDepthSteps(shadows.localSettings.format));
    for (uint32_t i = 0; i < kMaxShadowCascades; ++i)
    {
        constants.shadowCascade[i] = glm::vec4(0.f);
        constants.shadowViewProjection[i] = glm::mat4(1.f);
    }
    for (uint32_t i = 0; i < cascadeCount && i < kMaxShadowCascades; ++i)
    {
        const ShadowCascade &cascade = shadows.fit->cascades[i];
        // w is the cascade's depth range in world units, which is what turns the
        // [0, 1] depths the shader compares back into metres. Only the margin
        // debug view reads it; the comparison itself never needs the scale.
        constants.shadowCascade[i] = glm::vec4(cascade.splitFarView,
                                               CascadeDepthBiasNdc(cascade, shadows.settings),
                                               CascadeNormalOffsetWorld(cascade, shadows.settings),
                                               cascade.depthRange);
        constants.shadowViewProjection[i] = cascade.viewProjection;
    }
    // z is the penumbra cap divided by the filter's radius, so the shader can
    // turn it into a tap step with one divide by the cascade's own depth range —
    // which is the same 2r the cascade's box is wide. Infinite for a filter with
    // no kernel of its own, so the min below it never bites.
    const float radiusTaps = FilterRadiusTaps(shadows.settings.filter);
    const float cappedStepNumerator =
        radiusTaps > 0.f ? kMaxPenumbraWorld / radiusTaps : std::numeric_limits<float>::max();
    // The cascade's depth range cancels: a blocker distance read in the map's own
    // [0, 1] depth, times this, is already the UV step that gives the penumbra
    // the sun's angular radius calls for at that distance.
    const float contactStepNumerator =
        radiusTaps > 0.f ? kSunPenumbraPerWorldUnit / radiusTaps : std::numeric_limits<float>::max();
    constants.shadowParams = glm::vec4(ShadowTexelSizeUv(shadows.settings), shadows.settings.cascadeBlend,
                                       cappedStepNumerator, contactStepNumerator);

    constants.shadowPcss = glm::vec4(0.f);
    if (shadows.sunPcss)
    {
        const SunPcssConstants sunPcss = SunPcssFrameConstants(shadows.settings);
        constants.shadowPcss =
            glm::vec4(sunPcss.penumbraUvPerDepth, sunPcss.maxReachUv, sunPcss.texelDepth, kMaxPenumbraWorld);
    }

    commandList->writeBuffer(_frameConstantsBuffer, &constants, sizeof(constants));
}

void MeshPass::SetShadowMap(nvrhi::ITexture *cascades)
{
    _shadowMap = cascades;
}

void MeshPass::SetShadowAtlas(nvrhi::ITexture *atlas)
{
    _shadowAtlas = atlas;
}

void MeshPass::SetShadowViewTable(nvrhi::IBuffer *views)
{
    _shadowViewTable = views;
}

void MeshPass::SetEnvironment(nvrhi::ITexture *specularCube)
{
    _environmentCube = specularCube;
}

void MeshPass::SetAmbientOcclusion(nvrhi::ITexture *occlusion)
{
    _ambientOcclusion = occlusion;
}

bool MeshPass::CreateBrdfTable()
{
    nvrhi::TextureDesc tableDesc;
    tableDesc.width = kBrdfLutSize;
    tableDesc.height = kBrdfLutSize;
    // Half floats: every channel is a fraction in [0, 1], which half precision
    // holds to a part in two thousand, and a half-float format is one every
    // Vulkan device can filter — a 32-bit float one is not.
    tableDesc.format = nvrhi::Format::RGBA16_FLOAT;
    tableDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    tableDesc.keepInitialState = true;
    tableDesc.debugName = "MeshPass::BrdfTable";
    _brdfTable = _device->createTexture(tableDesc);
    if (_brdfTable == nullptr)
    {
        Core::Log::Error("MeshPass: failed to create the BRDF table.");
        return false;
    }

    // Built here rather than on demand because every lit surface reads it: the
    // per-light lobe's energy compensation comes from its third channel.
    const std::vector<BrdfTableTexel> table = BuildBrdfLut(kBrdfLutSize, kBrdfLutSampleCount);
    std::vector<uint64_t> packed;
    packed.reserve(table.size());
    for (const BrdfTableTexel &texel : table)
    {
        packed.push_back(glm::packHalf4x16(glm::vec4(texel.environment, texel.directAlbedo, 0.f)));
    }

    nvrhi::CommandListHandle upload = _device->createCommandList();
    upload->open();
    upload->writeTexture(_brdfTable, 0, 0, packed.data(), kBrdfLutSize * sizeof(uint64_t));
    upload->close();
    _device->executeCommandList(upload);
    return true;
}

nvrhi::IBindingSet *MeshPass::GetOrCreateGlobalBindingSet(nvrhi::IBuffer *instanceBuffer) const
{
    if (_globalBindingSet != nullptr && _globalSetInstanceBuffer == instanceBuffer &&
        _globalSetShadowMap == _shadowMap && _globalSetShadowAtlas == _shadowAtlas &&
        _globalSetShadowViewTable == _shadowViewTable && _globalSetEnvironmentCube == _environmentCube &&
        _globalSetAmbientOcclusion == _ambientOcclusion)
    {
        return _globalBindingSet;
    }

    // Every referenced handle but the instance buffer and the cascade array is
    // stable for the pass's lifetime (frame CB / samplers owned here; the light
    // buffers, though rebuilt on a viewport change, keep their handles; the
    // material table is fixed capacity). So this rebuilds only on an
    // instance-buffer growth, a cascade reallocation, or an explicit
    // InvalidateBindingSets() — not per frame.
    nvrhi::BindingSetDesc bindingSetDesc;
    using nvrhi::BindingSetItem;
    nvrhi::IBuffer *const shadowViews =
        _shadowViewTable != nullptr ? _shadowViewTable : _noShadowViews.NativeBuffer();
    nvrhi::ITexture *const environment = _environmentCube != nullptr ? _environmentCube : _noEnvironmentCube.Get();
    nvrhi::ITexture *const occlusion =
        _ambientOcclusion != nullptr ? _ambientOcclusion : _noAmbientOcclusion.Get();
    bindingSetDesc.addItem(
        BindingSetItem::ConstantBuffer(std::to_underlying(ConstantBufferSlot::Frame), _frameConstantsBuffer));
    bindingSetDesc.addItem(BindingSetItem::Sampler(std::to_underlying(SamplerSlot::Material), _sampler));
    bindingSetDesc.addItem(
        BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::Materials), _materialTable));
    bindingSetDesc.addItem(BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::PointLights),
                                                                _clusterGrid->PointLightBuffer().NativeBuffer()));
    bindingSetDesc.addItem(BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::SpotLights),
                                                                _clusterGrid->SpotLightBuffer().NativeBuffer()));
    bindingSetDesc.addItem(BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::DirLights),
                                                                _clusterGrid->DirLightBuffer().NativeBuffer()));
    bindingSetDesc.addItem(BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::LightIndices),
                                                                _clusterGrid->LightIndexBuffer().NativeBuffer()));
    bindingSetDesc.addItem(BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::LightGrids),
                                                                _clusterGrid->LightGridBuffer().NativeBuffer()));
    bindingSetDesc.addItem(
        BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::Instances), instanceBuffer));
    bindingSetDesc.addItem(BindingSetItem::Texture_SRV(std::to_underlying(ResourceSlot::ShadowCascades), _shadowMap));
    bindingSetDesc.addItem(
        BindingSetItem::StructuredBuffer_SRV(std::to_underlying(ResourceSlot::ShadowViews), shadowViews));
    bindingSetDesc.addItem(BindingSetItem::Texture_SRV(std::to_underlying(ResourceSlot::ShadowAtlas), _shadowAtlas));
    bindingSetDesc.addItem(BindingSetItem::Sampler(std::to_underlying(SamplerSlot::Shadow), _shadowSampler));
    bindingSetDesc.addItem(BindingSetItem::Texture_SRV(std::to_underlying(ResourceSlot::Environment), environment));
    bindingSetDesc.addItem(BindingSetItem::Texture_SRV(std::to_underlying(ResourceSlot::BrdfTable), _brdfTable));
    bindingSetDesc.addItem(BindingSetItem::Sampler(std::to_underlying(SamplerSlot::Clamp), _clampSampler));
    bindingSetDesc.addItem(BindingSetItem::Texture_SRV(std::to_underlying(ResourceSlot::ScreenOcclusion), occlusion));
    _globalBindingSet = _device->createBindingSet(bindingSetDesc, _bindingLayout);
    _globalSetInstanceBuffer = instanceBuffer;
    _globalSetShadowMap = _shadowMap;
    _globalSetShadowAtlas = _shadowAtlas;
    _globalSetShadowViewTable = _shadowViewTable;
    _globalSetEnvironmentCube = _environmentCube;
    _globalSetAmbientOcclusion = _ambientOcclusion;
    return _globalBindingSet;
}

void MeshPass::EnsureIndirectCapacity(uint32_t commandCount) const
{
    if (_indirectBuffer != nullptr && commandCount <= _indirectCapacity)
    {
        return;
    }
    const uint32_t grown = std::max(_indirectCapacity * 2u, kInitialInstanceCapacity);
    const uint32_t capacity = std::max(grown, commandCount);

    nvrhi::BufferDesc desc;
    desc.byteSize = static_cast<uint64_t>(sizeof(nvrhi::DrawIndexedIndirectArguments)) * capacity;
    desc.isDrawIndirectArgs = true;
    desc.initialState = nvrhi::ResourceStates::IndirectArgument;
    desc.keepInitialState = true;
    desc.debugName = "MeshPass::IndirectArgs";
    _indirectBuffer = _device->createBuffer(desc);
    _indirectCapacity = capacity;
}

MeshPass::SubmitStats MeshPass::Submit(const RenderFrame &frame, std::span<const DrawItem> items,
                                       MeshPassStage stage) const
{
    nvrhi::ICommandList *const commandList = frame.commandList;
    SubmitStats stats;

    // Build the frame's per-instance records and indirect commands in one pass over
    // the (already sorted) items. Each valid item appends one instance record — the
    // vertex shader reads it via gl_InstanceIndex — and either extends the current
    // batch or opens a new one. A batch is a maximal run of consecutive items with
    // the same geometry (mesh + submesh) *and* the same pipeline; its records are
    // contiguous (instanceIndex increments per item), so one instanced draw covers
    // them. Material differs per instance (read from the record), so it never splits
    // a batch — but the pipeline is one draw's worth of state, so it must.
    //
    // Reused members, not locals: clear() keeps the capacity, so a steady-state
    // scene allocates nothing here.
    std::vector<InstanceData> &instances   = _scratchInstances;
    std::vector<nvrhi::DrawIndexedIndirectArguments> &commands    = _scratchCommands;
    // Parallel to `commands`: the mesh each batch draws, for the arena vertex/index
    // buffers at record time (they bind via GraphicsState, not the indirect args),
    // and the pipeline it draws through.
    std::vector<const MeshBuffer *> &batchMeshes = _scratchBatchMeshes;
    std::vector<MeshPipeline> &batchPipelines = _scratchBatchPipelines;
    instances.clear();
    commands.clear();
    batchMeshes.clear();
    batchPipelines.clear();
    instances.reserve(items.size());

    const MeshBuffer *prevMesh    = nullptr;
    uint32_t prevSubmesh = UINT32_MAX;
    MeshPipeline prevPipeline = MeshPipeline::Opaque;
    uint32_t instanceIndex = 0;
    for (const DrawItem &item : items)
    {
        if (item.material == nullptr || item.mesh == nullptr)
        {
            continue; // producer should have filtered these; belt and suspenders
        }
        // The shader indexes `materials[]` (an unbounded SSBO runtime array) with
        // this id, so the GPU cannot bounds-check it — an id past the table reads
        // whatever memory follows. AssetCache::MintMaterialId saturates to keep
        // that from happening; catch a violation here, in debug, before it ships
        // to the GPU where it would be silent.
        ASSISI_ASSERT(item.material->Id() < AssetCache::kMaxMaterials,
                      "material id indexes past the material table — the shader read would be out of bounds");
        instances.push_back(InstanceData{item.model, item.material->Id()});

        const MeshPipeline pipeline = SortKeyPipeline(item.sortKey);
        // The key's pipeline field is 8 bits wide but names only kMeshPipelineCount
        // of them, so a key built by hand rather than by MakeOpaqueSortKey could
        // index past the pipeline array. Catch that here rather than at the bind.
        ASSISI_ASSERT(static_cast<uint32_t>(pipeline) < kMeshPipelineCount,
                      "draw item's sort key names a pipeline the pass does not have");
        if (!commands.empty() && item.mesh == prevMesh && item.submeshIndex == prevSubmesh &&
            pipeline == prevPipeline)
        {
            ++commands.back().instanceCount; // same geometry and pipeline as the run — coalesce
        }
        else
        {
            // Arena addressing: index values stay mesh-local, so baseVertexLocation
            // shifts them into this mesh's vertex range and startIndexLocation picks
            // its slice of the shared index buffer. startInstanceLocation makes the
            // batch's first gl_InstanceIndex select its first record.
            const Geometry::SubMesh &subMesh = item.mesh->SubMeshes()[item.submeshIndex];
            nvrhi::DrawIndexedIndirectArguments cmd;
            cmd.indexCount            = subMesh.IndexCount;
            cmd.instanceCount         = 1;
            cmd.startIndexLocation    = item.mesh->IndexBase() + subMesh.IndexOffset;
            cmd.baseVertexLocation    = static_cast<int32_t>(item.mesh->VertexBase());
            cmd.startInstanceLocation = instanceIndex;
            commands.push_back(cmd);
            batchMeshes.push_back(item.mesh);
            batchPipelines.push_back(pipeline);
            prevMesh     = item.mesh;
            prevSubmesh  = item.submeshIndex;
            prevPipeline = pipeline;
        }
        ++instanceIndex;
    }
    if (commands.empty())
    {
        return stats; // nothing to draw — leave the instance/indirect buffers untouched
    }
    stats.instances = static_cast<uint32_t>(instances.size());
    stats.batches   = static_cast<uint32_t>(commands.size());

    // Grow the instance buffer to hold this frame's records if needed. A growth
    // swaps the buffer handle, so the cached global set (which references it) must
    // be rebuilt — GetOrCreateGlobalBindingSet notices via _globalSetInstanceBuffer.
    if (!_instanceBuffer.IsValid() || instances.size() > _instanceBuffer.CapacityElements())
    {
        const uint32_t needed = static_cast<uint32_t>(instances.size());
        const uint32_t grown = std::max(_instanceBuffer.CapacityElements() * 2u, kInitialInstanceCapacity);
        _instanceBuffer.Create(_device, sizeof(InstanceData), std::max(grown, needed), /*allowUnorderedAccess=*/ false,
                               "MeshPass::Instances");
    }
    EnsureIndirectCapacity(static_cast<uint32_t>(commands.size()));

    // Upload both buffers before recording draws: the copies land in this command
    // list ahead of the draws that read them, so the ordering is correct in-frame.
    _instanceBuffer.Upload(commandList, instances.data(), static_cast<uint32_t>(instances.size()));
    commandList->writeBuffer(_indirectBuffer, commands.data(),
                             commands.size() * sizeof(nvrhi::DrawIndexedIndirectArguments));

    _lastSubmission = Submission::Cpu;
    stats.drawCalls = RecordCpuDraws(frame, stage).drawCalls;
    return stats;
}

MeshPass::SubmitStats MeshPass::RecordCpuDraws(const RenderFrame &frame, MeshPassStage stage) const
{
    nvrhi::ICommandList *const commandList = frame.commandList;
    SubmitStats stats;
    const std::vector<nvrhi::DrawIndexedIndirectArguments> &commands = _scratchCommands;
    const std::vector<const MeshBuffer *> &batchMeshes = _scratchBatchMeshes;
    const std::vector<MeshPipeline> &batchPipelines = _scratchBatchPipelines;
    const nvrhi::GraphicsPipelineHandle *const pipelines = PipelinesFor(stage);

    nvrhi::IBindingSet *const globalBindingSet = GetOrCreateGlobalBindingSet(_instanceBuffer.NativeBuffer());

    // Multi-draw each maximal run of batches that share the same arena vertex/index
    // buffers *and* pipeline with one drawIndexedIndirect. Stage C keeps every mesh
    // in one arena, so an all-opaque frame is a single call spanning all batches;
    // the grouping stays correct if a second arena (divergent vertex format) is ever
    // added, and a frame with cutouts costs one extra call for the masked run.
    const uint32_t commandCount = static_cast<uint32_t>(commands.size());
    uint32_t runStart     = 0;
    while (runStart < commandCount)
    {
        nvrhi::IBuffer *const vertexBuffer = batchMeshes[runStart]->VertexBuffer();
        nvrhi::IBuffer *const indexBuffer  = batchMeshes[runStart]->IndexBuffer();
        const MeshPipeline pipeline = batchPipelines[runStart];
        uint32_t runEnd       = runStart + 1;
        while (runEnd < commandCount && batchMeshes[runEnd]->VertexBuffer() == vertexBuffer &&
               batchMeshes[runEnd]->IndexBuffer() == indexBuffer && batchPipelines[runEnd] == pipeline)
        {
            ++runEnd;
        }

        nvrhi::GraphicsState state;
        state.pipeline = pipelines[static_cast<uint32_t>(pipeline)];
        state.framebuffer = frame.framebuffer;
        state.addBindingSet(globalBindingSet); // set 0: frame/sampler/lights/material table/instances
        state.addBindingSet(_bindlessTable);   // set 1: bindless textures
        state.viewport.addViewportAndScissorRect(
            nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
        state.addVertexBuffer(nvrhi::VertexBufferBinding{vertexBuffer, 0, 0});
        state.indexBuffer = nvrhi::IndexBufferBinding{indexBuffer, nvrhi::Format::R32_UINT, 0};
        state.indirectParams = _indirectBuffer;
        commandList->setGraphicsState(state);

        commandList->drawIndexedIndirect(runStart * sizeof(nvrhi::DrawIndexedIndirectArguments), runEnd - runStart);
        ++stats.drawCalls;

        runStart = runEnd;
    }

    return stats;
}

MeshPass::SubmitStats MeshPass::SubmitIndirect(const RenderFrame &frame, const IndirectDrawInputs &in,
                                               MeshPassStage stage) const
{
    _lastSubmission = Submission::Indirect;
    _lastIndirect = in;
    return RecordIndirectDraws(frame, in, stage);
}

MeshPass::SubmitStats MeshPass::Redraw(const RenderFrame &frame, MeshPassStage stage) const
{
    switch (_lastSubmission)
    {
    case Submission::Cpu:      return RecordCpuDraws(frame, stage);
    case Submission::Indirect: return RecordIndirectDraws(frame, _lastIndirect, stage);
    case Submission::None:     break;
    }
    return SubmitStats{};
}

MeshPass::SubmitStats MeshPass::RecordIndirectDraws(const RenderFrame &frame, const IndirectDrawInputs &in,
                                                    MeshPassStage stage) const
{
    SubmitStats stats;
    uint32_t totalCommands = 0;
    for (const uint32_t count : in.commandCounts)
    {
        totalCommands += count;
    }
    if (in.indirectBuffer == nullptr || in.instanceBuffer == nullptr || in.vertexBuffer == nullptr ||
        in.indexBuffer == nullptr || totalCommands == 0)
    {
        return stats; // nothing culled this frame (or the culler is unavailable)
    }

    nvrhi::ICommandList *const commandList = frame.commandList;

    // Bind the global set against the cull pass's instance buffer (t6) — the
    // records it wrote are read by gl_InstanceIndex just like the CPU path's.
    nvrhi::IBindingSet *const globalBindingSet = GetOrCreateGlobalBindingSet(in.instanceBuffer);

    // One multi-draw per live pipeline block. Single arena → all batches share the
    // arena's vertex/index buffers (stage C/E), so each block is one
    // drawIndexedIndirect over a contiguous command range; empty batches carry
    // instanceCount 0 and draw nothing. A frame that places only ordinary opaque
    // materials has one block and issues exactly the one call it always did.
    nvrhi::GraphicsState state;
    state.framebuffer  = frame.framebuffer;
    state.addBindingSet(globalBindingSet); // set 0
    state.addBindingSet(_bindlessTable);   // set 1: bindless textures
    state.viewport.addViewportAndScissorRect(
        nvrhi::Viewport(static_cast<float>(frame.width), static_cast<float>(frame.height)));
    state.addVertexBuffer(nvrhi::VertexBufferBinding{in.vertexBuffer, 0, 0});
    state.indexBuffer    = nvrhi::IndexBufferBinding{in.indexBuffer, nvrhi::Format::R32_UINT, 0};
    state.indirectParams = in.indirectBuffer;

    uint32_t firstCommand = 0;
    for (uint32_t p = 0; p < kMeshPipelineCount; ++p)
    {
        const uint32_t count = in.commandCounts[p];
        if (count == 0)
        {
            continue; // a pipeline nothing was placed for has no block to draw
        }
        state.pipeline = PipelinesFor(stage)[p];
        commandList->setGraphicsState(state);
        commandList->drawIndexedIndirect(firstCommand * sizeof(nvrhi::DrawIndexedIndirectArguments), count);
        ++stats.drawCalls;
        firstCommand += count;
    }

    // Survivor/batch tallies come from the culler's readback; report the calls.
    return stats;
}

} // namespace Assisi::Render
