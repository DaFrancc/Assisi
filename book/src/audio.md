# Audio

Every game with a window has audio. The engine opens the player's sound device
when the game starts, and everything the game plays goes through one **mixer**.
A dedicated server has no audio at all.

If no sound device will open, the game still runs, silently. Sounds still start
and finish on time, so nothing that waits for one gets stuck.

## Buses

The mixer groups sounds into **buses**. Each bus has its own volume, and a
sound's final volume is its bus's volume times the volume of every bus above
it. Turning down `SFX` turns down every sound effect at once; turning down
`Master` turns down everything.

Every game has these buses:

```text
Master
├── Music
├── SFX
├── Ambient
├── UI
└── Voice
```

| Bus | What goes in it |
|---|---|
| `Master` | Everything, through the buses below it. |
| `Music` | Music tracks. |
| `SFX` | Sound effects: footsteps, gunshots, impacts. |
| `Ambient` | Background sound: wind, rain, crowds. |
| `UI` | Menu clicks and hovers. |
| `Voice` | Dialogue and voice chat. |

Volumes go from 0 (silent) to 1 (full). A volume change fades over a fiftieth
of a second rather than jumping, so moving a slider never clicks.

## Your own buses

To give a group of sounds its own volume, declare a bus in
`assets/config/buses.json`:

```json
{
  "version": 1,
  "type": "BusConfig",
  "buses": [
    { "name": "Footsteps", "parent": "SFX", "volume": 0.8 },
    { "name": "Gravel", "parent": "Footsteps" }
  ]
}
```

| Field | What it does |
|---|---|
| `name` | The bus's name. It can't repeat another bus's name, including a default one. |
| `parent` | The bus this one feeds into: a default bus, or one declared **earlier** in the list. |
| `volume` | The volume it starts at, from 0 to 1. Defaults to 1. |

If the file breaks one of these rules, the game logs a warning and uses only
the default buses. A game can declare up to 58 buses of its own.

## Volumes the player sets

A bus's volume is the player's: there is one way to set it, through the
player's settings, so nothing in the game can overwrite what they chose.
Systems reach the settings through `ctx.settings`.

### Showing the volumes

A settings screen lists the buses and shows each at its current volume:

```cpp
for (const std::string_view bus : ctx.settings->Buses())
{
    const float volume = ctx.settings->BusVolume(bus).value_or(1.0f);
    // Show a slider named `bus`, set to `volume`.
}
```

- `Buses()` gives every bus the game has, the default ones and your own, each
  parent before the buses under it.
- `BusVolume(bus)` gives the player's volume for that bus if they set one, and
  the volume `buses.json` gives it otherwise. It's empty for a name the game has
  no bus for.

### Changing a volume

A volume changes when the player chooses one, so the system that changes it
reacts to an [event](events.md) rather than doing work every frame. Your
settings screen pushes the event when the player confirms a volume:

```cpp
AEVENT()
struct BusVolumeChosen
{
    Assisi::Core::InternedString bus;
    float volume = 1.0f;
};
```

and a system applies and saves whatever was chosen:

```cpp
void ApplyChosenVolumesSystem(Assisi::App::SystemContext &ctx)
{
    const Assisi::Core::EventSpan<BusVolumeChosen> chosen = ctx.events.Read<BusVolumeChosen>();
    if (chosen.size() == 0 || ctx.settings == nullptr)
    {
        return;
    }
    for (const BusVolumeChosen &choice : chosen)
    {
        ctx.settings->Options().busVolumes[std::string{choice.bus.View()}] = choice.volume;
    }
    ctx.settings->ApplyAudio();
    ctx.settings->Save();
}
```

On a frame where nothing was chosen, which is almost every frame, the system
returns at once. Otherwise it takes three steps:

1. `ctx.settings->Options().busVolumes[...] = choice.volume;` records each
   choice in the options held in memory. `busVolumes` maps bus names to
   volumes, and holds only the buses the player has set. Nothing sounds
   different yet.
2. `ctx.settings->ApplyAudio();` hands every volume in `busVolumes` to the mixer,
   which fades each bus to it. A name the game has no bus for is skipped and
   logged. In a dedicated server, which has no audio, this does nothing.
3. `ctx.settings->Save();` writes the options to `options.json`, so the volume
   survives a restart. When the game starts, it reads the file and applies the
   volumes itself.

The steps are separate so a screen can change several settings and apply them
together, try a volume while a slider moves without saving it, and save once
when the screen closes.

`ctx.settings` is null only where systems run without a game around them, such
as in tests. Check it first, as you would `ctx.ui`.

Lowering the music for a cutscene is the game's choice, not the player's, so it
isn't done by changing a bus's volume. A way to do that on top of the player's
volume, without changing it, is planned.

Volumes are saved in `options.json` by bus name, and only for buses the player
has changed. Every other bus keeps the volume `buses.json` gives it, even if you
change that volume in a later version:

```json
{
    "audio": {
        "volumes": {
            "Music": 0.5
        }
    }
}
```

A saved volume for a bus the game no longer has is kept in the file but not
used, so it comes back if the bus does.

## Sound files

The engine reads three formats:

| Format | Good for |
|---|---|
| `.wav` | Short sounds played often, such as clicks and footsteps. It is not compressed, so it plays with no decoding cost but takes the most space. |
| `.ogg` (Ogg Vorbis) | Long sounds such as music and ambience, where size matters most. |
| `.flac` | Long sounds that must stay lossless. |

A file ships in the format you authored it in; the engine never converts one.
The extension must be lowercase.

MP3 is not supported. Convert an MP3 to Ogg Vorbis or FLAC first, with a tool
such as [ffmpeg](https://ffmpeg.org) or [Audacity](https://www.audacityteam.org):

```sh
ffmpeg -i music.mp3 -c:a libvorbis -q:a 6 music.ogg
```

Decoding is done by [miniaudio](https://github.com/mackron/miniaudio), and Ogg
Vorbis by the [stb_vorbis](https://github.com/nothings/stb) copy that ships
with it.

### Sounds are assets

A sound file in `assets/` is an asset of the `sound` kind. When the editor
first sees a `.wav`, `.flac` or `.ogg`, it writes that kind into the file's
`.aast` sidecar.

The cook decodes every sound once to check it. A file that does not decode
fails the cook, naming the file, instead of failing in a player's game. A sound
that decodes ships exactly as you authored it.

A component refers to a sound by its id, like any other asset:

```cpp
AFIELD() Assisi::Core::AssetId clip;
```

A system loads it from `ctx.assets`. The first request starts loading the file
in the background and returns nothing, so ask again on later frames:

```cpp
std::shared_ptr<const Assisi::Audio::PcmClip> clip = ctx.assets->Resolve<Assisi::Audio::PcmClip>(id);
if (clip == nullptr)
{
    return; // still loading, missing, or broken
}
```

A missing or broken sound logs one warning and is not loaded again.

## Playing sounds

Sounds are played by entities with an audio emitter. `ctx.mixer` is what the
engine's emitter system plays them through; it starts and stops sounds, and
has no way to change a bus's volume.

A loaded clip plays on a bus through the mixer:

```cpp
(void)ctx.mixer->Attach(clip, Assisi::Audio::ToBusId(Assisi::Audio::DefaultBus::Sfx));
```

`ctx.mixer` is null in a host with no audio device, so check it first.
