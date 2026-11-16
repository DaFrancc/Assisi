# Tutorial: a blur

In this tutorial you add a **blur**, the shape most post effects share: it
reads the picture and writes a changed one. That needs what the
[vignette](render-pass-vignette.md) avoided:

- reading an image, through a texture and a sampler;
- an image of the pass's own to draw into, since a draw can't read the image
  it writes;
- remaking that image and everything that refers to it when the picture is
  reallocated;
- two draws, timed separately;
- a switch that turns the pass off, and freeing its memory while it is.

Do the [vignette tutorial](render-pass-vignette.md) first; this one assumes you
know what a pipeline, a binding layout and a binding set are.

## How the blur works

Each pixel becomes a weighted average of its neighbours. Averaging a square of
9 by 9 pixels takes 81 reads a pixel. A **Gaussian blur** gives the same
result in two steps: average 9 pixels across, then 9 pixels down, for 18
reads. So the pass draws twice:

1. **Across:** read the scene image, write the result into a **scratch image**
   the pass owns.
2. **Down:** read the scratch image, write the result back into the scene
   image.

The scratch image is what lets the pass read one image while writing another
in each step.

## Step 1: Write the shader

Create `assets/shaders/blur.frag`. One shader does both steps; a push constant
says which direction to sample in.

```glsl
#version 450

// One step of a Gaussian blur: each pixel becomes a weighted average of its
// neighbours along one direction. Run across, then down, it blurs both ways.

layout(location = 0) in vec2 vTexCoords;
layout(location = 0) out vec4 outColor;

// The image to blur. A texture's slot number is written as it is; a
// sampler's is its slot plus 128.
layout(binding = 0) uniform texture2D uSource;
layout(binding = 128) uniform sampler uSampler;

layout(push_constant) uniform PushConstants
{
    // From one sample to the next, in texture coordinates.
    vec2 step;
} pc;

// Each sample's share of the average, the centre first. Both sides get the
// same shares, and all nine add up to 1.
const float kWeights[5] =
    float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

vec3 Sample(vec2 position)
{
    return texture(sampler2D(uSource, uSampler), position).rgb;
}

void main()
{
    vec3 sum = Sample(vTexCoords) * kWeights[0];
    for (int i = 1; i < 5; ++i)
    {
        vec2 offset = pc.step * float(i);
        sum += Sample(vTexCoords + offset) * kWeights[i];
        sum += Sample(vTexCoords - offset) * kWeights[i];
    }
    outColor = vec4(sum, 1.0);
}
```

A **texture** is an image a shader reads. A **sampler** says how to read it:
whether a position between two pixels blends them, and what lies past the
edge. `texture(sampler2D(uSource, uSampler), position)` reads the texture at a
position from (0, 0) to (1, 1); `Sample` wraps it so the loop reads easily.

### Slot numbers

Each input sits in a numbered slot, and the shader and the C++ must agree on
the numbers. NVRHI numbers each kind of input from 0, and in the shader adds
an offset per kind, so two kinds never share a number:

| Input | In the shader | In the C++ binding layout and set |
|---|---|---|
| Texture, or read-only buffer | `binding = N` | `Texture_SRV(N)`, `StructuredBuffer_SRV(N)` |
| Sampler | `binding = 128 + N` | `Sampler(N)` |
| Constant buffer | `binding = 256 + N` | `ConstantBuffer(N)` |
| Read-write texture or buffer | `binding = 384 + N` | `Texture_UAV(N)`, `StructuredBuffer_UAV(N)` |
| Push constants | `push_constant` | `PushConstants(0, size)` |

So the shader's `binding = 128` is the sampler in slot 0, which the C++ calls
`Sampler(0)`.

## Step 2: Add a switch

The blur runs only while something asks for it. The frame's settings reach
every pass through `AppFrame`, in `modules/App/include/Assisi/App/AppPasses.hpp`.
Add a field at the end of it:

```cpp
struct AppFrame
{
    // ... the fields already there ...

    /// The blur runs.
    bool blurEnabled = false;
};
```

`Application::RenderFrame`, in `modules/App/src/Application.cpp`, fills in the
`AppFrame` each frame. Add the new field last, since C++ wants the fields in
the order the struct declares them:

```cpp
    const Passes::AppFrame appFrame{
        .tonemap = _options.tonemap,
        // ... the fields already there ...
        .blurEnabled = true,
    };
```

A real effect would read a player setting here instead of `true`; see
[Graphics settings](rendering-settings.md) for where those live.

## Step 3: Declare the pass

In `AppPasses.hpp`, above `VignettePass` if you kept it, otherwise above
`TonemapPass`:

```cpp
/// @brief Blurs the scene: across into a scratch image of its own, then
/// down from that back into the scene image.
APASS(name = "blur", after = "pp-resolve",
      timers = {"blur-across", "blur-down"})
struct BlurPass
{
    bool Initialize(const AppPassInit &init);
    bool Wanted(const AppFrame &frame) const { return frame.blurEnabled; }
    void Release();
    void Record(Recorder &rec, const AppFrame &frame, Writes<SceneHdr> out);

    // Helpers, each shown below.
    void Rebuild(const Render::RenderTarget &image);
    void MakeScratch(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] nvrhi::BindingSetHandle Reading(nvrhi::ITexture *source) const;
    [[nodiscard]] nvrhi::GraphicsPipelineHandle
    MakePipeline(const nvrhi::FramebufferInfo &target) const;
    void DrawStep(nvrhi::ICommandList *commandList,
                  nvrhi::IGraphicsPipeline *pipeline,
                  nvrhi::IFramebuffer *target,
                  nvrhi::IBindingSet *source,
                  glm::vec2 step) const;

    // Made once, by Initialize.
    nvrhi::DeviceHandle device;
    nvrhi::ShaderHandle vertexShader;
    nvrhi::ShaderHandle pixelShader;
    nvrhi::SamplerHandle sampler;
    nvrhi::BindingLayoutHandle layout;

    // Made again for each new scene image, by Rebuild.
    nvrhi::TextureHandle scratch;
    nvrhi::FramebufferHandle scratchFramebuffer;
    nvrhi::BindingSetHandle readImage;
    nvrhi::BindingSetHandle readScratch;
    nvrhi::GraphicsPipelineHandle intoScratch;
    nvrhi::GraphicsPipelineHandle intoImage;
    /// The scene image the members above were made for.
    nvrhi::ITexture *builtFor = nullptr;
};
```

`timers = {"blur-across", "blur-down"}` gives each step its own timer, so a
capture shows which one costs what. A pass can carry helper functions like
these; the build only looks for `Record` and the hooks.

The members split in two. The shader, the sampler and the layout never change
once made. Everything after them refers to the scene image, directly or
through its format, and has to be made again when the scene image is
replaced.

## Step 4: Load the shader and describe its inputs

In `modules/App/src/AppPasses.cpp`, with
`#include <Assisi/Render/ShaderModule.hpp>` among the includes:

```cpp
namespace
{
/// How far apart the blur's samples are, in pixels. At 2, its nine samples
/// reach eight pixels either side.
constexpr float kBlurSpread = 2.0f;
} // namespace

bool BlurPass::Initialize(const AppPassInit &init)
{
    device = init.device;
    vertexShader = init.fullscreenVertexShader;
    pixelShader = Render::LoadSpirvShader(device, "shaders/blur.frag.spv",
                                          nvrhi::ShaderType::Pixel);
    if (pixelShader == nullptr)
    {
        Core::Log::Warn("Application: blur unavailable (its shader failed to load).");
        return false;
    }

    // Filtered, so a sample between two pixels blends them; clamped, so the
    // samples past the picture's edge repeat the edge instead of wrapping.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true);
    samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    sampler = device->createSampler(samplerDesc);

    // The shader's inputs, slot by slot, matching its layout(binding = ...).
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(glm::vec2)));
    layout = device->createBindingLayout(layoutDesc);
    return sampler != nullptr && layout != nullptr;
}
```

The layout says "slot 0 holds a texture, sampler slot 0 holds a sampler, and
there is a `vec2` of push constants": the shader's three declarations.

## Step 5: Make what depends on the scene image

`Rebuild` makes everything that refers to the scene image, using three
helpers:

```cpp
void BlurPass::Rebuild(const Render::RenderTarget &image)
{
    MakeScratch(image.width, image.height);
    readImage = Reading(image.colorTexture);
    readScratch = Reading(scratch);
    intoScratch = MakePipeline(scratchFramebuffer->getFramebufferInfo());
    intoImage = MakePipeline(image.framebuffer->getFramebufferInfo());
    builtFor = image.colorTexture;
}
```

### The scratch image

```cpp
void BlurPass::MakeScratch(std::uint32_t width, std::uint32_t height)
{
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = Render::kSceneColorFormat;
    desc.isRenderTarget = true;
    desc.isShaderResource = true;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;
    desc.keepInitialState = true;
    desc.debugName = "BlurPass::Scratch";
    scratch = device->createTexture(desc);

    nvrhi::FramebufferDesc framebufferDesc;
    framebufferDesc.addColorAttachment(scratch);
    scratchFramebuffer = device->createFramebuffer(framebufferDesc);
}
```

- It is the scene image's size, in its format, `kSceneColorFormat`, which
  holds light values above 1, so the blur keeps the brightness the tone map
  needs later.
- `isRenderTarget` lets it be drawn into; `isShaderResource` lets a shader
  read it.
- You never tell the graphics card when an image switches from being drawn
  into to being read. NVRHI tracks each image's state and inserts the wait
  itself; `initialState` is the state it starts in, and `keepInitialState`
  returns it there at the end of each frame.
- A **framebuffer** wraps the image so it can be drawn into.

### The binding sets

```cpp
nvrhi::BindingSetHandle BlurPass::Reading(nvrhi::ITexture *source) const
{
    // The layout's slots, now with what fills them.
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, source));
    desc.addItem(nvrhi::BindingSetItem::Sampler(0, sampler));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(glm::vec2)));
    return device->createBindingSet(desc, layout);
}
```

There are two, `readImage` and `readScratch`, because the two steps read
different images. A binding set fixes which texture fills the slot, so a
different texture needs a different set.

### The pipelines

```cpp
nvrhi::GraphicsPipelineHandle
BlurPass::MakePipeline(const nvrhi::FramebufferInfo &target) const
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
    return device->createGraphicsPipeline(desc, target);
}
```

There are two, `intoScratch` and `intoImage`, because a pipeline is built for
the format of the image it draws into, and the two differ: the scratch image
has no depth buffer, and without MSAA the scene image carries one.

## Step 6: Draw the two steps

`DrawStep` records one full-screen draw:

```cpp
void BlurPass::DrawStep(nvrhi::ICommandList *commandList,
                        nvrhi::IGraphicsPipeline *pipeline,
                        nvrhi::IFramebuffer *target,
                        nvrhi::IBindingSet *source,
                        glm::vec2 step) const
{
    const nvrhi::FramebufferInfoEx &size = target->getFramebufferInfo();
    const nvrhi::Viewport viewport(static_cast<float>(size.width),
                                   static_cast<float>(size.height));
    nvrhi::GraphicsState state;
    state.pipeline = pipeline;
    state.framebuffer = target;
    state.addBindingSet(source);
    state.viewport.addViewportAndScissorRect(viewport);

    commandList->setGraphicsState(state);
    commandList->setPushConstants(&step, sizeof(step));
    commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
}
```

`Record` makes sure everything fits the scene image, then draws twice:

```cpp
void BlurPass::Record(Recorder &rec, const AppFrame & /*frame*/, Writes<SceneHdr> out)
{
    const Render::RenderTarget &image = out.Get<SceneHdr>().target;
    if (image.colorTexture != builtFor)
    {
        Rebuild(image);
    }

    // The distance between samples: a pixel's width or height, in texture
    // coordinates, times the spread.
    const glm::vec2 across(kBlurSpread / static_cast<float>(image.width), 0.0f);
    const glm::vec2 down(0.0f, kBlurSpread / static_cast<float>(image.height));

    rec.Begin("blur-across");
    DrawStep(rec.CommandList(), intoScratch, scratchFramebuffer, readImage, across);
    rec.Begin("blur-down");
    DrawStep(rec.CommandList(), intoImage, image.framebuffer, readScratch, down);
}
```

- **The check at the top.** Resizing the window, or switching MSAA, replaces
  the scene image with a new one. `readImage` would still point at the old
  one, so the pass makes everything again. Comparing the texture is enough,
  because a new image is a new texture.
- **The timers.** `rec.Begin("blur-across")` opens that timer, and
  `rec.Begin("blur-down")` closes it and opens the next. The names must be
  ones the `APASS` lists.

## Step 7: Free the memory while it is off

```cpp
void BlurPass::Release()
{
    scratch = nullptr;
    scratchFramebuffer = nullptr;
    readImage = nullptr;
    readScratch = nullptr;
    intoScratch = nullptr;
    intoImage = nullptr;
    builtFor = nullptr;
}
```

The engine calls `Release` every frame `Wanted` says no. Dropping the handles
frees the scratch image, as big as the screen, for a player who has the
effect off. Setting `builtFor` to null makes the next frame that wants the
blur rebuild everything. What `Initialize` made stays: it is small, and
making it again would be slow.

## Step 8: Build and see it

Run `./assisi build`. If you kept the vignette, the build stops:

```text
reflectgen: error: blur and vignette both write SceneHdr, and nothing orders
them. Say which goes first with after = "..." or before = "...".
```

Both now come after the resolve and write the same image, and blurring before
or after darkening gives different pictures. Darken last: change the
vignette's annotation to `APASS(name = "vignette", after = "blur")` and build
again. The printed order:

```text
reflectgen: 15 App pass(es): clear-targets -> scene -> pp-resolve -> blur
    -> vignette -> pp-exposure -> ...
```

Start the editor on `levels/Lights.alvl`. The scene is blurred and darkened at
the edges, while the editor's icons and wire shapes stay sharp, because the
overlays draw after the tone map. With the profiler's per-pass timing on, the
capture lists `blur-across`, `blur-down` and `vignette`.

Now set `.blurEnabled = false` in `Application.cpp` and build again. The blur
is gone, its timers are gone from captures, and its scratch image is freed.

When you are done, delete both passes, their shaders and the `AppFrame`
field.

## What to change for your own effect

- **A different effect on the whole picture:** keep the structure and change
  the shader. Anything that reads the picture and writes a new one fits: a
  sharpen, a colour grade, a chromatic aberration.
- **After the tone map instead of before:** harder today. With no FXAA and no
  overlays, the tone map writes straight onto the screen, and a shader can't
  read the screen. An effect there needs `RouteChain`, in
  `modules/Render/include/Assisi/Render/PostProcess.hpp`, to send the tone map
  into an image whenever the effect is on, as it already does for FXAA. Those
  images hold display values, 8 bits a channel, so make the scratch image in
  their format: `image.framebuffer->getFramebufferInfo().colorFormats[0]`.
- **Reading depth:** the scene's depth is `SceneTarget`'s `target.depthTexture`.
  Without MSAA, bind it like any texture. With MSAA it holds several samples a
  pixel, and the shader must declare it `texture2DMS` and read one sample with
  `texelFetch`; the scene distance pass, `SceneDistancePass.cpp`, builds a
  shader for each case.
- **More inputs:** add slots to the layout and to `Reading`, following the
  slot table, and mirror them in the shader.

The [next example](render-pass-exposure.md) walks through a real pass that
uses compute shaders and keeps data from one frame to the next.
