# Your own asset kinds

Every file in `assets/` is an asset with an id. What the engine does with a file
depends on its **kind**: what the data represents, such as a sound. A kind can
have several **formats**, the file types that encode it: a sound can be a
`.wav`, a `.flac` or an `.ogg`.

The engine has kinds of its own: meshes, textures, materials, levels, shaders,
fonts and screens. A module can add more without changing the engine. This page
shows how to add one, and how a file says which kind it is.

## Which kind a file is

A format is not a use. A `.png` can be a colour texture, a heightmap or a table
of data, so the engine never decides a file's kind from its extension. Every
file's `.aast` sidecar states it:

```json
{
  "guid": "1c7fa8f9-bd8e-4f5d-b402-edadb14dc57d",
  "type": "AssetSidecar",
  "uses": [{ "kind": "texture" }],
  "version": 1
}
```

- **New files get their kind written for them.** When the editor creates a
  sidecar, it writes the kind that reads that format. Where several kinds read
  it, it writes the one that prefers the format, or the first by name when none
  or several do. A later change to that choice only affects files imported
  afterwards.
- **You change it with "Use as".** Right-click a file in the asset browser. The
  menu lists only the kinds that can read its format, with the current one
  marked, and appears only when there is more than one.
- **The cook refuses a file it cannot place,** naming the file: a sidecar with
  no kind, a kind this build does not have, a kind that cannot read the file's
  format, or a texture a material binds whose sidecar names another kind.
- **One kind per file.** A file needed as two kinds is two files for now.

Two sorts of file have no kind of their own and need none: parts of another
asset, such as a glTF's `.bin`, a font's `.ttf` or a shader's GLSL source, and
compiled `.spv` shaders, which are build outputs with no committed sidecar.

## What the engine does for a kind

Once a kind is registered, every file whose sidecar names it:

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

Add a source file to your module that registers the kind: its name, the formats
it reads, and a function that turns the file's bytes into the value. The
function runs on a worker thread, so it must not touch anything the main thread
changes.

```cpp
#include <Assisi/Core/AssetKind.hpp>

namespace
{

using Assisi::Core::AssetError;
using Assisi::Core::AssetErrorCode;

/// What went wrong, as every asset says it, with the decoder's words as detail.
AssetError ToAssetError(AudioError error)
{
    const AssetErrorCode code = error == AudioError::UnsupportedEncoding ? AssetErrorCode::UnsupportedEncoding
                                                                         : AssetErrorCode::CorruptAsset;
    return AssetError{code, ToString(error)};
}

std::expected<PcmClip, AssetError> LoadSound(std::span<const std::byte> bytes)
{
    std::expected<PcmClip, AudioError> clip = DecodeClip(bytes);
    if (!clip)
    {
        return std::unexpected(ToAssetError(clip.error()));
    }
    return std::move(*clip);
}

using Assisi::Core::AssetFormat;

[[maybe_unused]] const bool kRegistered = Assisi::Core::AssetKindRegistry::Instance().Register(
    Assisi::Core::MakeAssetKind<PcmClip>("sound",
                                         {AssetFormat{.extension = ".wav", .preferred = true},
                                          AssetFormat{.extension = ".flac", .preferred = true},
                                          AssetFormat{.extension = ".ogg", .preferred = true}},
                                         LoadSound));

} // namespace
```

- **The name is the kind's identity.** It is what sidecars write, so two kinds
  cannot share one, and it cannot be the name of one of the engine's own kinds.
- **Extensions are matched exactly.** `.WAV` is not `.wav`.
- **`preferred` says what new files get.** Several kinds may read one format. A
  heightmap kind would read `.png` without preferring it, so new `.png` files
  stay textures and a designer marks the heightmaps with "Use as".
- **Errors are `AssetError`s.** Every kind reports failure the same way, so
  whatever loads an asset can tell what happened without knowing its kind. Map
  your decoder's errors onto a code: `CorruptAsset` for a file whose contents are
  broken, `UnsupportedEncoding` for a valid file this build cannot read. Put the
  decoder's own description in `detail`; the warning a failed load logs prints
  both.
- **`detail` is a view.** Point it at text that is never freed, such as a string
  literal or an error enum's `ToString`. Text built at runtime would be gone by
  the time the warning prints it.

If the value needs work on the main thread before it can be used, such as
creating GPU resources, set the kind's `finish` before registering it, with
`MakeAssetFinish<PcmClip>(yourFinish)`. It runs once, on the main thread, after
the load and before anything sees the value.

## Step 3: a cook step, if the kind needs one

Without a cook step, a file ships exactly as it was authored. Add one to check a
file before it ships, or to convert it into something faster to load. It goes in
a separate source file, because a shipped game never cooks and must not link
cook code. It reports errors the way the load does, so declare `ToAssetError` in
a header both files include:

```cpp
#include <Assisi/Core/AssetKind.hpp>

namespace
{

/// Raise when the step would cook the same file differently.
constexpr std::uint32_t kSoundCookVersion = 1;

std::expected<std::vector<std::byte>, Assisi::Core::AssetError> CookSound(std::span<const std::byte> source)
{
    // Decode once and throw the samples away: a file that would never play
    // fails the cook instead of a player's game.
    if (std::expected<PcmClip, AudioError> clip = DecodeClip(source); !clip)
    {
        return std::unexpected(ToAssetError(clip.error()));
    }
    return std::vector<std::byte>{source.begin(), source.end()};
}

[[maybe_unused]] const bool kRegistered = Assisi::Core::AssetKindRegistry::Instance().RegisterCookStep(
    Assisi::Core::MakeAssetCookStep(Assisi::Core::AssetKindId{"sound"}, kSoundCookVersion, CookSound));

} // namespace
```

Raise `version` whenever the step would cook the same file differently, so the
cook redoes files it has already cooked. A step's error fails the cook with the
file's path, the code and your detail:

```
cook: sounds/bad.wav: the file's contents are corrupt (the audio could not be decoded)
```

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
