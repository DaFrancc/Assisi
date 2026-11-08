# Example: automatic exposure

This page walks through one of the engine's own passes, `pp-exposure`, which
adjusts the picture's brightness the way your eyes adjust walking from a dark
room into sunlight. It uses what the two tutorials didn't:

- **compute shaders**, which run work on the graphics card without drawing;
- **read-write buffers**, which a shader can change;
- data the graphics card keeps from one frame to the next;
- two pieces of work where the second reads what the first wrote;
- a buffer handed to another pass.

There is nothing to type. Read it alongside the code, after the
[blur tutorial](render-pass-blur.md). The excerpts below are wrapped to fit
the page, and a comment marks where a part of a file is left out.

## What it does

Each frame, the pass:

1. Sorts the picture's pixels by brightness into a **histogram**: 128 counters,
   one for each band of brightness, each counting the pixels in its band.
2. Works out the picture's typical brightness from the middle of that
   histogram, leaving out the darkest half and the brightest 5% of pixels, so
   a bright lamp or a black shadow doesn't swing it.
3. Sets a target exposure that would bring that brightness to a comfortable
   level.
4. Moves the exposure it keeps a little toward the target, so the change takes
   a moment rather than jumping.

The tone map then draws the picture at that exposure. A
[`PostProcessVolume`](rendering-post-processing.md) turns it on by making some
of the exposure automatic.

Nothing comes back to the processor. The histogram, the kept exposure and the
tone map's use of it all stay on the graphics card, so the frame never waits
to read a number back.

## Where the code is

| File | What it holds |
|---|---|
| `modules/App/src/AppPasses.cpp` | `ExposureMeterPass`, the pass |
| `modules/Render/src/ExposurePass.cpp` | `Render::ExposurePass`: the buffers and the two dispatches |
| `assets/shaders/exposure_histogram.comp` | Step 1 |
| `assets/shaders/exposure_adapt.comp` | Steps 2 to 4 |
| `modules/Render/include/Assisi/Render/Exposure.hpp` | The same steps in C++, which tests check; the shaders copy them |

## The pass

```cpp
bool ExposureMeterPass::Record(Recorder &rec, const AppFrame &frame,
                               Reads<SceneHdr, SceneLook> in, Writes<Exposure> out)
{
    const SceneLook &look = in.Get<SceneLook>();
    if (look.passthrough || look.settings.exposure.automatic <= 0.0f)
    {
        return false;
    }
    const Render::RenderTarget &scene = in.Get<SceneHdr>().target;
    if (scene.colorTexture != meteredImage)
    {
        meter.Configure(scene.colorTexture, scene.width, scene.height);
        meteredImage = scene.colorTexture;
    }
    meter.Run(rec.CommandList(), look.settings.exposure, frame.frameSeconds);
    out.Get<Exposure>() = Exposure{.state = meter.State()};
    return true;
}
```

Three things to notice:

- **It decides inside `Record` whether to run.** Whether a level wants
  automatic exposure comes from `SceneLook`, which the scene pass produces
  during the frame, so `Wanted`, which only sees the frame's settings, can't
  know. Returning `false` switches the pass off for the frame: the engine
  empties `Exposure`, and the tone map draws at the level's fixed exposure.
- **It rebinds when the picture is replaced**, as the blur does, by comparing
  textures.
- **It hands on a buffer**, not a number. `meter.State()` is a buffer on the
  graphics card holding the kept exposure, and `Exposure` passes it to the tone
  map.

## Buffers that outlive the frame

`ExposurePass::Initialize`, in `ExposurePass.cpp`, makes two buffers, once,
with a helper beside it:

```cpp
nvrhi::BufferHandle MakeStorage(nvrhi::IDevice *device, uint32_t stride,
                                uint32_t count, const char *name)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = static_cast<uint64_t>(stride) * count;
    desc.structStride = stride;
    desc.canHaveUAVs = true;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

bool ExposurePass::Initialize(nvrhi::IDevice *device,
                              const std::string &histogramShaderSpvPath,
                              const std::string &adaptShaderSpvPath)
{
    // ... the two compute shaders, their binding layouts and the sampler ...

    _bins = MakeStorage(_device, sizeof(uint32_t), kExposureHistogramBins,
                        "Exposure::Histogram");
    _state = MakeStorage(_device, sizeof(ExposureState), 1u, "Exposure::State");

    // ... the second step's binding set, which only needs these buffers ...
}
```

A **UAV** ("unordered access view") is NVRHI's name for a read-write
resource; `canHaveUAVs` lets shaders write the buffer. `structStride` makes it
a **structured buffer**, an array of one struct, which a shader declares as a
`buffer` block.

- `_bins` is the histogram: 128 counters, cleared every frame.
- `_state` holds one `ExposureState`: the kept exposure, and whether it has
  been set yet. It is cleared once, the first time the pass runs, and never
  again, so each frame starts from the last one's exposure. That is what makes
  the change gradual.

## Step 1: the histogram, in a compute shader

A **compute shader** runs a function many times in parallel, like a pixel
shader, but on whatever you say rather than on the pixels a draw covers. The
runs are grouped into **workgroups**, a fixed number of runs each, set in the
shader; you start the work with a **dispatch** that says how many workgroups
to run.

The main part of `exposure_histogram.comp`:

```glsl
// The push constants, kBins (128), kLumaWeights and Bin(), which turns a
// brightness into its band's number, are declared above this in the file.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0)   uniform texture2D uScene;
layout(binding = 128) uniform sampler   uSceneSampler;
layout(std430, binding = 384) buffer Histogram { uint bins[]; };

shared uint localBins[kBins];

void main()
{
    uint local = gl_LocalInvocationIndex;
    if (local < kBins)
    {
        localBins[local] = 0u;
    }
    barrier();

    uvec2 cell = gl_GlobalInvocationID.xy;
    if (all(lessThan(cell, pc.sampleGrid)))
    {
        vec2 uv = (vec2(cell) * 2.0 + 1.0) * pc.texelSize;
        vec3 radiance = textureLod(sampler2D(uScene, uSceneSampler), uv, 0.0).rgb;
        atomicAdd(localBins[Bin(dot(radiance, kLumaWeights))], 1u);
    }
    barrier();

    if (local < kBins && localBins[local] != 0u)
    {
        atomicAdd(bins[local], localBins[local]);
    }
}
```

Each workgroup is 16 by 16 runs, and each run reads one point of the picture:
the middle of a 2 by 2 block of pixels, where the sampler's blending averages
all four. The histogram buffer is slot 0 of the read-write kind, so 384 in the
shader, as the [slot table](render-pass-blur.md#slot-numbers) gives.

Thousands of runs add to the same 128 counters at once. Two runs adding to one
counter at the same moment would lose a count, so they use `atomicAdd`, which
the graphics card guarantees happens one at a time. Atomics on the shared
buffer in graphics memory are slow, so each workgroup first counts into
`shared` memory, a small fast store its 256 runs share, and then adds its
totals to the buffer once per band. `barrier()` makes every run in the
workgroup wait until all of them reach it: the first so no run counts into
`localBins` before it is cleared, the second so none copies it out before
everyone has counted.

`gl_GlobalInvocationID` numbers the runs across all workgroups, and the
`lessThan` check skips the runs past the picture's edge, since the picture is
rarely a whole number of workgroups wide.

## Step 2: metering and adapting

The main part of `exposure_adapt.comp`:

```glsl
// The push constants, kTargetLuminance and Metered(), which averages the
// middle of the histogram, are declared in the file too.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(std430, binding = 384) readonly buffer Histogram { uint bins[]; };
layout(std430, binding = 385) buffer ExposureState
{
    float autoStops;
    uint  seeded;
} state;

void main()
{
    float target = log2(kTargetLuminance) - Metered() + pc.compensationStops;
    target = clamp(target, pc.minStops, pc.maxStops);

    float rate = max(pc.adaptationRate, 0.0);
    float covered = 1.0 - exp(-rate * max(pc.seconds, 0.0));
    float current = state.seeded != 0u ? state.autoStops : target;
    state.autoStops = current + (target - current) * covered;
    state.seeded = 1u;
}
```

This one runs once: a workgroup of one, dispatched once. The histogram is 128
numbers, which one run reads faster than many runs could share out.
`Metered()` averages the middle of the histogram, and the target follows from
it. The last lines move the kept exposure toward the target by a share that
depends on how long the frame took, so the adjustment takes the same time at
30 frames a second as at 144. The first frame, with nothing kept yet, takes
the target at once.

The two buffers are read-write slots 0 and 1, so 384 and 385.

## Recording the two dispatches

`ExposurePass::Run` records both, each frame the pass runs:

```cpp
void ExposurePass::Run(nvrhi::ICommandList *commandList,
                       const ExposureSettings &settings, float seconds)
{
    if (!_histogramBindings || !_adaptBindings)
    {
        return;
    }

    if (!_stateCleared)
    {
        commandList->clearBufferUInt(_state, 0u);
        _stateCleared = true;
    }
    commandList->clearBufferUInt(_bins, 0u);

    const ExposureSettings safe = Sanitized(settings);
    const glm::uvec2 grid = glm::max(_sceneExtent / kSampleBlock, glm::uvec2(1u));
    const ExposureConstants constants{.sampleGrid = grid,
                                      .texelSize = 1.0f / glm::vec2(_sceneExtent),
                                      .minStops = safe.minStops,
                                      .maxStops = safe.maxStops,
                                      .compensationStops = safe.compensationStops,
                                      .adaptationRate = safe.adaptationRate,
                                      .seconds = seconds};

    const glm::uvec2 groups =
        (grid + glm::uvec2(kHistogramGroupSide - 1u)) / kHistogramGroupSide;
    _histogram.Dispatch(commandList, _histogramBindings, groups.x, groups.y, 1u,
                        &constants, sizeof(constants));
    _adapt.Dispatch(commandList, _adaptBindings, 1u, 1u, 1u,
                    &constants, sizeof(constants));
}
```

- **Clearing:** the histogram is emptied every frame; the kept exposure only
  the first time, which marks it as not set yet.
- **How many workgroups:** one run per 2 by 2 block makes a grid of half the
  picture's size, and the dispatch rounds it up to whole workgroups of 16.
- **`Render::ComputeShader`** (in `ComputeShader.hpp`) holds a compute shader's
  pipeline and binding layout, and `Dispatch` sets the state, pushes the
  constants and dispatches. Use it for a compute pass of your own.
- **The order between the dispatches:** the second reads the histogram the
  first writes. NVRHI sees both use `_bins` as a read-write buffer and makes
  the second wait for the first, as it waits between draws.
- **One set of push constants for both:** `ExposureConstants`, in
  `Exposure.hpp`, mirrors the shaders' `PushConstants` block field by field.
  The C++ struct and the GLSL block must have the same fields, in the same
  order, of the same types; a `vec2` must start at a multiple of 8 bytes, so
  the struct puts its two `vec2`s first.

## Handing the buffer to the tone map

The tone map reads the kept exposure through `Exposure`, as a read-only
buffer in its slot 2:

```glsl
layout(std430, binding = 2) readonly buffer ExposureState
{
    float autoStops;
    uint  seeded;
} exposure;
```

The same buffer is read-write in the compute shader and read-only here.
NVRHI switches it between the two uses itself. While automatic exposure is
off, `Exposure` is empty, and the tone map binds a stand-in buffer in that
slot, because every slot of a binding set must hold something. The shader
never reads the slot unless the metering ran.

## Where to look next

| Pass | What it shows |
|---|---|
| `pp-bloom`, `Render::BloomPass` | A chain of images, each half the size of the last, drawn down the chain and back up, each step up blending the smaller image over the larger |
| `scene-distance` and `ssao`, `Render::SceneDistancePass` and `Render::SsaoPass` | A distance image made from the depth buffer, with a shader for MSAA depth; a constant buffer at slot 256; an occlusion step and a blur step timed separately |
| `clouds`, `Render::CloudPass` | Compute shaders that fill 3D noise textures and their smaller copies, then march through them into the sky's cloud map |
| `sky`, `Render::SkyPass` | A scene pass run once for each camera view, drawn only where the scene left pixels empty |
