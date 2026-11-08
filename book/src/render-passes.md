# Adding a render pass

A **render pass** is one job the graphics card does each frame, such as drawing
the shadow maps or the tone map; [How a frame is drawn](rendering-frame.md)
lists them all. This page shows how to add one to the engine: what to write,
how the engine fits it into the frame, and what the build checks for you.

The passes are part of the engine, so adding one means editing engine source.
A game can't add passes of its own yet.

This page covers how passes fit into the frame. Three pages after it cover
writing the GPU work inside one, each building on the last:

1. [Tutorial: a vignette](render-pass-vignette.md): a pass with a shader of
   its own, which draws over the picture in place.
2. [Tutorial: a blur](render-pass-blur.md): a pass that reads the picture,
   keeps an image of its own, draws twice, and frees its memory while off.
3. [Example: automatic exposure](render-pass-exposure.md): the engine's own
   exposure pass, with compute shaders and data kept from frame to frame.

## Where the passes are declared

Each pass is a C++ struct marked `APASS`. The frame's passes live in two
headers, split by the module that runs them:

| Header | Its passes | Bodies go in |
|---|---|---|
| `modules/Runtime/include/Assisi/Runtime/ScenePasses.hpp` | The scene: skinning, shadows, sky, lights, meshes | `modules/Runtime/src/ScenePasses.cpp` |
| `modules/App/include/Assisi/App/AppPasses.hpp` | Everything around the scene: the clear, post-processing, the overlays, the UI | `modules/App/src/AppPasses.cpp` |

The scene's passes run inside the `scene` pass of the second header, so the
frame is still one list in order.

You never call a pass yourself. When you build, a code generator called
**reflectgen** reads each header, works out the order the passes must run in,
checks it, and writes the C++ that runs them every frame. The build prints the
order it found, on one line; here it is wrapped to fit:

```text
reflectgen: 13 App pass(es): clear-targets -> scene -> pp-resolve
    -> pp-exposure -> pp-bloom -> pp-tonemap -> overlays -> pp-overlay-resolve
    -> pp-fxaa -> pp-blit -> game-ui -> frame-capture -> ui
```

## Example: the MSAA resolve

This is the whole of the pass that combines the scene's MSAA samples into one
colour per pixel. The declaration, in `AppPasses.hpp`:

```cpp
/// @brief Averages the scene's samples, as radiance, into the single-sampled
/// image the post steps read.
APASS(name = "pp-resolve")
struct ResolvePass
{
    bool Wanted(const AppFrame &frame) const
    {
        return frame.route.ResolvesScene();
    }
    void Record(Recorder &rec, const AppFrame &frame,
                Reads<SceneTarget> in, Writes<SceneHdr> out);
};
```

And its body, in `AppPasses.cpp`:

```cpp
void ResolvePass::Record(Recorder &rec, const AppFrame & /*frame*/,
                         Reads<SceneTarget> in, Writes<SceneHdr> out)
{
    nvrhi::ITexture *samples = in.Get<SceneTarget>().target.colorTexture;
    nvrhi::ITexture *resolved = out.Get<SceneHdr>().target.colorTexture;
    rec.CommandList()->resolveTexture(resolved, nvrhi::AllSubresources,
                                      samples, nvrhi::AllSubresources);
}
```

Line by line:

- **`APASS(name = "pp-resolve")`** marks the struct as a pass. The name is what
  a profiler capture calls it, and what other passes use to say "run after
  me".
- **`Wanted`** says whether the pass runs this frame. Without MSAA there are no
  samples to combine, so it says no. A pass with no `Wanted` runs every frame.
- **`Record`** does the work. Its parameters say what the pass uses:
  `Reads<SceneTarget>` means it reads the image the scene was drawn into, and
  `Writes<SceneHdr>` means it writes the image the post steps read. The engine
  runs it after whatever writes `SceneTarget`, and before whatever reads
  `SceneHdr`.
- **`rec.CommandList()`** gives the command list to record GPU work into.
  **`in.Get<T>()`** and **`out.Get<T>()`** give the things the pass listed;
  asking for a type it didn't list fails to compile.

## Try it: paint the scene red

This pass fills the scene with red after it is drawn, so you can watch a pass
of your own go into the frame. Remove it when you are done.

### Step 1: Declare the pass

In `modules/App/include/Assisi/App/AppPasses.hpp`, add this above
`TonemapPass`:

```cpp
/// @brief Paints the whole scene red.
APASS(name = "tint")
struct TintPass
{
    void Record(Recorder &rec, const AppFrame &frame, Writes<SceneHdr> out);
};
```

### Step 2: Write what it does

In `modules/App/src/AppPasses.cpp`, add the body:

```cpp
void TintPass::Record(Recorder &rec, const AppFrame & /*frame*/, Writes<SceneHdr> out)
{
    const nvrhi::Color red(1.0f, 0.0f, 0.0f, 1.0f);
    rec.CommandList()->clearTextureFloat(out.Get<SceneHdr>().target.colorTexture,
                                         nvrhi::AllSubresources, red);
}
```

### Step 3: Build, and read the error

Run `./assisi build`. It stops with:

```text
reflectgen: error: pp-resolve and tint both write SceneHdr, and nothing orders
them. Say which goes first with after = "..." or before = "...".
```

Two passes now write the same image, and which one runs first changes the
picture: painting before the resolve would be painted over. The build won't
guess. Say that the tint comes after the resolve:

```cpp
APASS(name = "tint", after = "pp-resolve")
```

Build again. The printed order now has the tint where you put it, and the
passes that read `SceneHdr` after it:

```text
reflectgen: 14 App pass(es): clear-targets -> scene -> pp-resolve -> tint
    -> pp-exposure -> pp-bloom -> pp-tonemap -> ...
```

### Step 4: See it

Start the editor with `./assisi run editor -- -l levels/Lights.alvl`. The
level is solid red, with the editor's light icons and wire shapes still drawn
on top, because the overlays come after the tone map. If you open the
[profiler](rendering-debugging.md#the-profiler) with per-pass timing on, `tint`
is listed as a pass of its own.

### Step 5: Remove it

Delete the struct and its body, and build again.

## What a pass reads and writes

The things passes hand each other are called **resources**. A resource is a
small struct holding what one pass makes and another uses: a texture, a buffer,
a list of draws. There are two kinds.

| Kind | Declared | Who writes it | While its writer is off |
|---|---|---|---|
| **Produced** | Anywhere the header can see, such as `SceneResources.hpp` | Exactly one pass | Emptied, so a pass reading it sees nothing rather than last frame's |
| **Shared** | Listed as a member of the header's imports struct, `SceneImports` or `AppImports` | Any number of passes, in an order the build can tell | Kept |

Shared resources are objects the engine owns and hands the passes every frame:
the images the post steps draw into, and services such as the mesh pass or the
lighting system. Everything else is produced. For example, the clouds pass
produces a `CloudMap`:

```cpp
struct CloudMap
{
    Render::CloudShadowConstants shadowConstants;
    glm::dvec2 drift{0.0};
    nvrhi::ITexture *sky = nullptr;
    nvrhi::ITexture *shadow = nullptr;
};
```

It fills it in with `out.Get<CloudMap>() = CloudMap{...};`, and the sky probe
and the sky read it with `Reads<CloudMap>`. When clouds are switched off, the
map is emptied, its textures are null, and the passes reading it draw without
clouds.

The rules:

- A pass that reads something runs after every pass that writes it.
- `Writes<>` may also read what it lists, which is how a pass draws on top of
  an image. Don't list a type in both `Reads<>` and `Writes<>`.
- Inside `Reads<>` and `Writes<>`, name each type without its namespace. The
  header brings the types it needs in with `using` near its top, such as
  `using Render::PostProcess;`.

## Choosing the order

Most of the order comes from reads and writes, so you rarely state it. Say it
yourself when the order matters but no data shows it:

- **`after = "name"`** runs this pass after the one named. The sky is
  `after = "draw-scene"`: it draws only where the scene left pixels empty, so
  the scene must be drawn first.
- **`before = "name"`** runs it before the one named.
- When several passes write the same shared resource, the build needs to know
  their order, as Step 3 showed.

When nothing decides between two passes, they keep the order they are written
in the header.

## Turning a pass on and off

A pass is **off** for a frame when its `Wanted` says no, or when its `Record`
returns `false`. Use `Wanted` when the frame's settings decide, and return
`false` from `Record` when the pass only finds out while working, for example
when a texture fails to allocate. Either way, the engine calls its `Release`
and empties what it produces.

A **base** pass is one every frame runs, even in an empty level with every
feature off: the clear, the tone map, the lit pass. Mark it `base`:

```cpp
APASS(name = "pp-tonemap", base)
```

A base pass has no `Wanted`, and its `Record` returns `void`; the build refuses
either.

## Hooks

Besides `Record`, a pass can have any of these member functions. The engine
finds them by name, and one with the right name and the wrong parameters fails
the build.

| Hook | When it is called | Use it to |
|---|---|---|
| `bool Wanted(const SceneFrame &frame) const` | Before `Record`, each frame | Say whether the pass runs this frame |
| `void Release()` | Each frame the pass is off | Free what it only needs while on, such as its textures |
| `bool Initialize(const ScenePassInit &init)` | Once, at start-up | Load shaders and build pipelines. Return `false`, after logging why, and the pass stays off for the run |
| `bool OnTargetsChanged(const nvrhi::FramebufferInfo &targets)` | When the scene's image changes format or sample count, such as when MSAA is switched on | Rebuild pipelines built for the old one |

In `AppPasses.hpp` the types are `AppFrame` and `AppPassInit` instead. A scene
pass that runs once for each camera view also takes `const ViewFrame &view`,
after the frame, in both `Record` and, if it needs it, `Wanted`.

`Record`'s parameters always come in this order: `Recorder &rec`, the frame,
the view, `Reads<...>`, `Writes<...>`. Leave out any you don't use except the
frame.

## Timers

The engine times every pass on the graphics card and labels it for graphics
debuggers, with no code from you. The timer starts the first time the pass
calls `rec.CommandList()` or `rec.Frame(...)`, so a pass that records nothing
doesn't show up in a capture.

- To time two halves of a pass separately, list both names and start the
  second one yourself:

  ```cpp
  APASS(name = "ssao", timers = {"ssao-occlusion", "ssao-blur"})
  ```

  Then call `rec.Begin("ssao-blur")` between the halves.
- A pass whose work is done by code that times itself, such as `scene`, which
  runs all the scene's passes, is marked `untimed`. It shows as a label in a
  graphics debugger but has no timer.

## What the build checks

| Mistake | What the build says |
|---|---|
| A pass reads something no pass writes | `sky-probe reads CloudMap, which no pass writes.` |
| Two passes produce the same resource | `CloudMap is written by skin and clouds. A resource the passes produce has one writer...` |
| Two passes write a shared resource in no fixed order | `pp-resolve and tint both write SceneHdr, and nothing orders them.` |
| The passes need each other in a circle | `the passes cannot be ordered, because each of these must come before the next:` and then each step of the circle |
| `after` or `before` names no pass | `sky is ordered against 'draw-scnee', which no pass is named.` |
| Two passes share a name | `two passes are named 'tint': TintPass and RedPass.` |
| `Record`'s parameters are out of order, or one isn't a pass's | The error names the parameter and lists the order they go in |
| A hook has the right name and the wrong parameters | A compile error naming the hook and the shape it must have |

## Tests that cover a new pass

- **`Gpu-BasePasses`**, one of the GPU tests, runs the empty level `PerfBlank`
  with every feature off and checks that exactly the base passes record. An
  optional pass whose feature is off must say no in `Wanted`, or record
  nothing, or this test fails.
- **The other GPU tests** check that every level still looks the way you last
  accepted. If your pass changes the picture on purpose, accept the new
  pictures; see
  [Checking the renderer automatically](rendering-debugging.md#checking-the-renderer-automatically).

Then add the pass to [How a frame is drawn](rendering-frame.md), in its place
in the order, with its timer name in brackets.
