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

## Changing a volume from a system

Systems reach the mixer through `ctx.mixer`. It's null in a dedicated server,
which has no audio, so check it first, as you would `ctx.ui`:

```cpp
void DuckMusicSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.mixer == nullptr)
    {
        return;
    }
    const std::optional<Assisi::Audio::BusId> music = ctx.mixer->Layout().FindBus("Music");
    if (music)
    {
        ctx.mixer->SetBusVolume(*music, 0.2f);
    }
}
```

- `ctx.mixer->Layout().FindBus("Music")` looks the bus up by name and gives
  back its id, or nothing if the game has no bus with that name.
- `SetBusVolume` fades the bus to its new volume over a fiftieth of a second.
  Volumes outside 0 to 1 are clamped.

This lasts for the current run only. The next time the game starts, the bus is
back at the player's saved volume, or the one `buses.json` gives it.

## Volumes the player sets

A volume the player chooses in a settings screen is kept in their options.
Systems reach the options through `ctx.settings`:

```cpp
void SetMusicVolumeSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.settings == nullptr)
    {
        return;
    }
    ctx.settings->Options().busVolumes["Music"] = 0.5f;
    ctx.settings->ApplyAudio();
    ctx.settings->Save();
}
```

1. `ctx.settings->Options().busVolumes["Music"] = 0.5f;` records the choice in
   the options held in memory. `busVolumes` maps bus names to volumes, and holds
   only the buses the player has set. Nothing sounds different yet.
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
as in tests.

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

## Playing sounds

Sounds themselves are played by entities with an audio emitter, not by calling
the mixer directly.
