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

The engine loads it in the background when an emitter that plays it is placed.
A missing or broken sound logs one warning and is not loaded again.

## Emitters

Every sound comes from an entity with an `AudioEmitter`. The emitter says what
it plays and how; an event says when.

| Field | What it does |
|---|---|
| `clip` | The sound it plays. |
| `bus` | The bus it plays on, by name: `SFX`, `Music`, `UI`, or one of your own. |
| `volume` | Its own volume, from 0 to 1, on top of the bus's. A change reaches sounds already playing. |
| `looping` | Whether its sound starts again from the beginning when it ends. |
| `maxConcurrentSounds` | How many of its sounds play at once. |
| `whenFull` | What a request does when that many are already playing. See below. |
| `maxQueuedRequests` | How many requests wait when `whenFull` is `Queue`. |
| `ignoresWorldPause` | Whether it plays on while its world is paused. |
| `space` | `InEar`, played as it is, or `InWorld`, placed in the world. Both sound the same for now. |

A level or blueprint that places emitters names the `AudioEmitters` system,
which is what plays them.

### When an emitter is full

| `whenFull` | A request to an emitter already playing its maximum... |
|---|---|
| `Drop` | is dropped. |
| `Queue` | waits for a sound to end, up to `maxQueuedRequests`. Past that, it is dropped. |
| `ReplaceOldest` | fades out the sound that started first, and plays. |
| `ReplaceNewest` | fades out the sound that started last, and plays. |

A sound that is replaced fades out over a hundredth of a second rather than
cutting off, so for that moment the old and new sounds overlap. With
`maxConcurrentSounds` at 1, either `Replace` restarts the sound.

A request that arrives before the emitter's clip has finished loading waits like
a queued one, up to `maxQueuedRequests`.

## Playing sounds

A system plays, stops, pauses and resumes an emitter by pushing an
[event](events.md) that names its entity:

```cpp
ctx.events.Push(Assisi::App::PlaySound{.target = door});
```

| Event | What it does |
|---|---|
| `PlaySound` | Plays the emitter's clip, as its `whenFull` says. |
| `StopEmitter` | Fades out everything the emitter is playing and forgets what it has queued. |
| `PauseEmitter` | Fades everything the emitter is playing to silence, where it is. Sounds it starts while paused start silent. |
| `ResumeEmitter` | Fades them back in from where they paused. |

The `AudioEmitters` system answers them in `PostUpdate`, so push them from
`Update` or earlier. An event that names no entity, or an entity with no
`AudioEmitter`, logs a warning and does nothing.

Emitters belong to the world they are in, and these events name an entity
only. With several worlds playing sounds at once, an event can reach an entity
in the wrong one.

### When sounds end

- **A destroyed emitter's sounds stop.** They fade out as the entity goes. A
  sound meant to outlive its entity, such as an explosion as something is
  destroyed, plays from another entity.
- **A paused world pauses its emitters.** They resume when the world does. An
  emitter with `ignoresWorldPause`, and the default emitter below, play on. An
  emitter you paused with `PauseEmitter` stays paused until you resume it,
  whatever the world does.

## The default emitter

Sounds that belong to no place in the world, such as the UI's, play from one
emitter the scene sets aside for them. Give an entity both an `AudioEmitter`
and a `DefaultEmitter`, and find it from a system with
`Assisi::App::FindDefaultEmitter(ctx.world)`. The default emitter plays on while
its world is paused.

`FindDefaultEmitter` logs a warning and finds nothing when no entity has a
`DefaultEmitter`, when more than one does, or when the one that does has no
`AudioEmitter`.

The `BaseGameplay` blueprint places one, which plays a click on the `UI` bus.

### Sounds from the UI

A button pushes an event with nothing in it, so it cannot name an emitter.
Instead it pushes an event of your game's, and a system of yours answers it and
plays the sound. The pause menu's Resume button works this way:

```xml
<menu_button name="resume" on_click="Game::ResumeRequested">#pause:resume</menu_button>
```

```cpp
if (!ctx.events.Read<ResumeRequested>().empty())
{
    pause->Hide();
    if (const std::optional<Assisi::ECS::Entity> speaker = Assisi::App::FindDefaultEmitter(ctx.world))
    {
        ctx.events.Push(Assisi::App::PlaySound{.target = *speaker});
    }
}
```

The engine plays a sound exactly as its file is written. A sound that starts or
ends at full volume clicks as it starts and stops; give the file a short fade in
and out if it should not.
