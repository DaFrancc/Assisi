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

A settings menu sets a bus's volume in the player's options and applies it:

```cpp
app.GetOptions().busVolumes["Music"] = 0.5f;
app.ApplyAudioOptions();
app.GetOptions().SaveToJson();
```

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

## Reaching the mixer

`Application::GetMixer()` returns the mixer, or null in a dedicated server. Use
it to change a bus's volume while the game runs, for example to lower the music
during a cutscene:

```cpp
if (Assisi::Audio::Mixer *mixer = app.GetMixer())
{
    const std::optional<Assisi::Audio::BusId> music = mixer->Layout().FindBus("Music");
    if (music)
    {
        mixer->SetBusVolume(*music, 0.2f);
    }
}
```

This changes the volume for this run only. To save it, set it in the player's
options as shown above.

Sounds themselves are played by entities with an audio emitter, not by calling
the mixer directly.
