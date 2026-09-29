# Your own asset kinds

Every file in `assets/` is an asset with an id. What the engine does with a file
depends on its **kind**: what the data represents, such as a sound. A kind can
have several **formats**, the file types that encode it: a sound can be a
`.wav`, a `.flac` or an `.ogg`.

The engine has kinds of its own: meshes, textures, materials, levels, shaders,
fonts and screens. A module can add more without changing the engine. This page
shows how.

## What the engine does for a kind

Once a kind is registered, every file with one of its extensions:

- gets an id and a `.aast` sidecar, like any other asset;
- is cooked into the game's package, through the kind's own cook step if it has
  one, or as it is if not;
- loads by id in the background, in the game from the package and in the editor
  from the source file, cooked the same way on the fly;
- is loaded once and shared by everything that asks for it;
- logs one warning if it is missing or broken, and is not tried again;
- appears in the editor's asset browser, under the kind's name.

## Step 1: the value it loads into

Decide what the loaded asset is in memory. For a sound, that is the decoded
samples:

```cpp
struct PcmClip
{
    std::vector<float> samples;
};
```

## Step 2: register the kind

Add a source file to your module that registers the kind: its name, its
extensions, and a function that turns the file's bytes into the value. The
function runs on a worker thread, so it must not touch anything the main thread
changes.

```cpp
#include <Assisi/Core/AssetKind.hpp>

namespace
{

std::expected<PcmClip, std::string> LoadSound(std::span<const std::byte> bytes)
{
    std::expected<PcmClip, AudioError> clip = DecodeClip(bytes);
    if (!clip)
    {
        return std::unexpected(std::string{ToString(clip.error())});
    }
    return std::move(*clip);
}

[[maybe_unused]] const bool kRegistered = Assisi::Core::AssetKindRegistry::Instance().Register(
    Assisi::Core::MakeAssetKind<PcmClip>("sound", {".wav", ".flac", ".ogg"}, LoadSound));

} // namespace
```

- **The name is the kind's identity.** Two kinds cannot share one, and it cannot
  be the name of one of the engine's own kinds.
- **Extensions are matched exactly.** `.WAV` is not `.wav`. Two kinds cannot
  claim the same extension, and the engine's own extensions (`.png`, `.gltf`,
  `.amat` and the rest) are already taken.
- **The error is the reason.** It goes into the warning a failed load logs.

If the value needs work on the main thread before it can be used, such as
creating GPU resources, set the kind's `finish` function before registering it.
It runs once, on the main thread, after the load and before anything sees the
value.

## Step 3: a cook step, if the kind needs one

Without a cook step, a file ships exactly as it was authored. Add one to check a
file before it ships, or to convert it into something faster to load. It goes in
a separate source file, because a shipped game never cooks and must not link
cook code:

```cpp
#include <Assisi/Core/AssetKind.hpp>

namespace
{

std::expected<std::vector<std::byte>, std::string> CookSound(std::span<const std::byte> source)
{
    // Decode once and throw the samples away: a file that would never play
    // fails the cook instead of a player's game.
    if (std::expected<PcmClip, AudioError> clip = DecodeClip(source); !clip)
    {
        return std::unexpected(std::string{"does not decode: "} + std::string{ToString(clip.error())});
    }
    return std::vector<std::byte>{source.begin(), source.end()};
}

bool Register()
{
    Assisi::Core::AssetCookStep step;
    step.kind    = Assisi::Core::AssetKindId{"sound"};
    step.version = 1;
    step.cook    = CookSound;
    return Assisi::Core::AssetKindRegistry::Instance().RegisterCookStep(std::move(step));
}

[[maybe_unused]] const bool kRegistered = Register();

} // namespace
```

Raise `version` whenever the step would cook the same file differently, so the
cook redoes files it has already cooked. A step's error fails the cook, with
the file's path and your message.

## Step 4: tell the build

In the module's `CMakeLists.txt`, list the two files. The cook step is optional:

```cmake
assisi_asset_kind(
  TARGET Assisi-Audio
  LOAD   src/SoundKind.cpp
  COOK   src/SoundCook.cpp
)
```

Don't add these files to the module's own sources as well. The helper builds
them so the linker cannot drop them: a registration nothing calls would
otherwise vanish from the build without an error.

Kinds are added by engine modules. Code in `apps/game/src` can't register one
yet.

## Loading an asset

Anything with the application can ask for an asset by id:

```cpp
std::shared_ptr<const PcmClip> clip = app.GetAssets().Resolve<PcmClip>(soundId);
if (clip == nullptr)
{
    return; // still loading, missing, or broken
}
```

The first request starts the load and returns nothing. Ask again on a later
frame and the value is there. The type you ask for must be the one the kind
loads into, or you get nothing back.
