# Tutorial: a vignette

In this tutorial you add a pass with a shader of your own: a **vignette**,
which darkens the picture toward its edges. It is the smallest complete shader
pass, and every later example builds on what it shows: writing a shader,
loading it, describing its inputs, building a pipeline, and recording a draw.

Read [Adding a render pass](render-passes.md) first; this page assumes you know
how a pass is declared and ordered.

## The GPU objects a draw needs

A pass that draws with a shader works with four objects. Each has a page of
detail in a graphics API's manual, but this is all you need here:

| Object | What it is |
|---|---|
| **Shader** | A small program the graphics card runs. A **pixel shader** runs once for every pixel a draw covers and returns that pixel's colour. |
| **Binding layout** | The list of inputs a shader takes, slot by slot: which textures, buffers and constants, and of what kind. |
| **Binding set** | The things that fill those slots for a draw: this texture, that buffer. |
| **Pipeline** | Everything about a draw fixed in advance: its shaders, its binding layout, how it blends with what is already drawn, and the format of the image it draws into. |

Building a pipeline is slow, so a pass builds it once and reuses it. Drawing is
then cheap: set the pipeline and binding set, give any **push constants** (a
few values, at most 128 bytes, handed over with the draw itself), and draw.

Every full-screen pass uses the same vertex shader, `assets/shaders/fullscreen.vert`.
It draws one triangle bigger than the screen, so every pixel runs your pixel
shader once, and hands each pixel `vTexCoords`: where it is on the screen,
from (0, 0) at the top left to (1, 1) at the bottom right.

## Step 1: Write the shader

Create `assets/shaders/vignette.frag`. The build compiles every `.frag`, `.vert`
and `.comp` under `assets/` on its own, into a `.spv` file beside it.

```glsl
#version 450

// Darkens the picture toward its edges. It reads no image: the pipeline's
// blend multiplies what is already drawn by the colour this writes.

layout(location = 0) in vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants
{
    float strength; // 0 leaves the picture alone; 1 takes the corners to black
} pc;

void main()
{
    vec2 fromCentre = vTexCoords - 0.5;
    float fade = 1.0 - pc.strength * 2.0 * dot(fromCentre, fromCentre);
    outColor = vec4(vec3(fade), 1.0);
}
```

- `in vec2 vTexCoords` is what `fullscreen.vert` hands over.
- `out vec4 outColor` is the colour the pixel shader returns.
- `push_constant` declares the values the pass hands over with the draw.
- `fade` is 1 at the centre and falls toward the corners, where
  `dot(fromCentre, fromCentre)` is largest.

A shader can't read the image it is drawing into. This one doesn't need to:
it only returns a number to multiply the picture by, and the pipeline's blend,
in Step 4, does the multiplying.

## Step 2: Declare the pass

In `modules/App/include/Assisi/App/AppPasses.hpp`, above `TonemapPass`:

```cpp
/// @brief Darkens the picture toward its edges.
APASS(name = "vignette", after = "pp-resolve")
struct VignettePass
{
    bool Initialize(const AppPassInit &init);
    void Record(Recorder &rec, const AppFrame &frame, Writes<SceneHdr> out);

    [[nodiscard]] nvrhi::GraphicsPipelineHandle
    MakePipeline(const nvrhi::FramebufferInfo &target) const;

    nvrhi::DeviceHandle device;
    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::BindingSetHandle bindings;
    nvrhi::GraphicsPipelineHandle pipeline;
    /// The image format the pipeline was built for, which changes with the AA mode.
    nvrhi::FramebufferInfo builtFor;
};
```

It writes `SceneHdr`, the scene's picture before the tone map, so the vignette
darkens light values and the tone map then treats the result like any other
picture. The `*Handle` types are NVRHI's reference-counted pointers: the object
is freed when the last handle to it goes.

## Step 3: Load the shader and describe its inputs

In `modules/App/src/AppPasses.cpp`, add `#include <Assisi/Render/ShaderModule.hpp>`
to the includes, then:

```cpp
namespace
{
/// How dark the corners get: 0 leaves the picture alone, 1 takes them to black.
constexpr float kVignetteStrength = 0.8f;
} // namespace

bool VignettePass::Initialize(const AppPassInit &init)
{
    device = init.device;
    vertexShader = init.fullscreenVertexShader;
    pixelShader = Render::LoadSpirvShader(device, "shaders/vignette.frag.spv",
                                          nvrhi::ShaderType::Pixel);
    if (pixelShader == nullptr)
    {
        Core::Log::Warn("Application: no vignette (its shader failed to load).");
        return false;
    }

    // The shader's inputs, slot by slot: here only the push constant.
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(float)));
    layout = device->createBindingLayout(layoutDesc);

    // What fills those slots. A push constant's value is given at draw time,
    // so the set only says it is there.
    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(float)));
    bindings = device->createBindingSet(setDesc, layout);
    return bindings != nullptr;
}
```

`Initialize` runs once, when the renderer starts. Returning `false` keeps the
pass off for the whole run, so a missing shader costs the effect, not the game.
The layout's push constant is the size of the shader's `PushConstants` block:
one `float`.

## Step 4: Build the pipeline and draw

First the pipeline, made by a helper:

```cpp
nvrhi::GraphicsPipelineHandle
VignettePass::MakePipeline(const nvrhi::FramebufferInfo &target) const
{
    nvrhi::GraphicsPipelineDesc desc;
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.VS = vertexShader;
    desc.PS = pixelShader;
    desc.addBindingLayout(layout);
    desc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    // The scene image may carry the scene's depth buffer; leave it as it is.
    desc.renderState.depthStencilState.depthTestEnable = false;
    desc.renderState.depthStencilState.depthWriteEnable = false;
    // What is drawn there already, times the shader's output.
    desc.renderState.blendState.targets[0]
        .setBlendEnable(true)
        .setSrcBlend(nvrhi::BlendFactor::DstColor)
        .setDestBlend(nvrhi::BlendFactor::Zero);
    return device->createGraphicsPipeline(desc, target);
}
```

The **blend** decides how the shader's colour combines with the pixel already
there: the source factor `DstColor` times the shader's colour, plus the
destination factor `Zero` times the old colour. That is the old colour times
`fade`.

Then `Record`, which makes sure the pipeline fits the image and draws:

```cpp
void VignettePass::Record(Recorder &rec, const AppFrame & /*frame*/,
                          Writes<SceneHdr> out)
{
    const Render::RenderTarget &image = out.Get<SceneHdr>().target;
    const nvrhi::FramebufferInfo &format = image.framebuffer->getFramebufferInfo();
    // FramebufferInfo has == but no !=.
    if (pipeline == nullptr || !(builtFor == format))
    {
        pipeline = MakePipeline(format);
        builtFor = format;
    }

    const nvrhi::Viewport viewport(static_cast<float>(image.width),
                                   static_cast<float>(image.height));
    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = image.framebuffer;
    state.addBindingSet(bindings);
    state.viewport.addViewportAndScissorRect(viewport);

    nvrhi::ICommandList *commandList = rec.CommandList();
    commandList->setGraphicsState(state);
    commandList->setPushConstants(&kVignetteStrength, sizeof(kVignetteStrength));
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}
```

The pipeline is made here, on the first frame, rather than in `Initialize`,
because it has to match the image it draws into, and that image changes. A
**framebuffer** is an image prepared to be drawn into, and its `FramebufferInfo`
is its format. Without MSAA, `SceneHdr` is the image the scene drew into,
depth buffer and all; with MSAA, it is the single-sampled image the samples
were combined into, with no depth. Switching MSAA changes the format, so
`Record` compares and makes a new pipeline.

The rest records the draw: the **graphics state** names the pipeline, the
image to draw into, the binding set and the area of the image to cover. Then
the push constant goes over, and `draw` with three vertices draws the
full-screen triangle. That is the moment the shaders run, once the frame's
commands reach the graphics card.

## Step 5: Build and see it

Run `./assisi build`. The printed order has the vignette after the resolve:

```text
reflectgen: 14 App pass(es): clear-targets -> scene -> pp-resolve -> vignette
    -> pp-exposure -> ...
```

Start the editor with `./assisi run editor -- -l levels/Lights.alvl`. The
picture darkens toward its corners. Switch MSAA on and off in the
[F11 window](rendering-debugging.md#the-f11-window) and it stays: that is the
pipeline being rebuilt for the new format.
With the [profiler](rendering-debugging.md#the-profiler)'s per-pass timing on,
`vignette` is listed with its time.

Keep the vignette for the [next tutorial](render-pass-blur.md), which adds a
blur in front of it.
