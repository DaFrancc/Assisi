# Steam

This page takes a game from nothing to running on Steam: the player's Steam
name, the overlay, achievements, stats and rich presence. The engine does the
work; you download one thing from Valve, run one command, and type one number.

> Steam and Steamworks are trademarks of Valve Corporation. Assisi is not made,
> endorsed or supported by Valve.

## What you need

- **A Steam account** and the **Steam client** installed and signed in.
- **For your own game on Steam**, a Steamworks partner account. You sign up at
  [partner.steamgames.com](https://partner.steamgames.com/), agree to Valve's
  terms, and pay Valve's fee for each game you put on Steam ("Steam Direct").
  Valve then gives your game an **app ID**, a number like `2847560`.
- **To try everything first without any of that**, use app ID **480**. It's
  Valve's own test game, "Spacewar", which every developer can use. Steam will
  show you as playing Spacewar while you test.

## 1. Download the Steamworks SDK

The SDK is the part of Steam that goes inside a game. Valve doesn't let anyone
else hand it out, so you download your own copy:

1. Sign in at [partner.steamgames.com](https://partner.steamgames.com/).
   (You can sign in to download the SDK before you have a game.)
2. Open the **Steamworks SDK** download page from the site's menu, read the
   Steamworks SDK Access Agreement, and accept it.
3. Download the newest SDK. You need **version 1.65 or newer**. The file is
   named like `steamworks_sdk_165.zip`.

Keep the zip wherever you keep downloads. **Never commit it, or anything in it,
to git**: Valve's agreement lets you keep a copy on your own computer to build
your game, and nothing more. The engine's `.gitignore` already keeps it out.

## 2. Install it into your project

From the top folder of your project:

```bash
./assisi steam-sdk ~/Downloads/steamworks_sdk_165.zip
```

(Use the path where your download actually is. On Windows: `assisi steam-sdk`.)

This copies the only two parts of the SDK the engine uses, its header files and
its runtime library, into a `steamworks/` folder in your project, which git
ignores. It prints:

```text
Installed Steamworks SDK 1.65: 47 files in steamworks/ (headers and runtime libraries only).
```

Your next build notices the SDK by itself. To install a newer SDK later, run
the same command with the new download; it replaces the old one.

<details>
<summary>Installing it by hand instead</summary>

Unzip the download, then copy two folders out of its `sdk/` folder into a new
`steamworks/` folder at the top of your project:

| Copy this | To here |
|---|---|
| `sdk/public/steam/` | `steamworks/public/steam/` |
| `sdk/redistributable_bin/linux64/` | `steamworks/redistributable_bin/linux64/` |
| `sdk/redistributable_bin/win64/` (Windows) | `steamworks/redistributable_bin/win64/` |

</details>

## 3. Type your app ID

Open `apps/game/CMakeLists.txt` and find this line, under **Steam**:

```cmake
set(GAME_STEAM_APP_ID "")
```

Put your app ID between the quotes, or `480` to test:

```cmake
set(GAME_STEAM_APP_ID "480")
```

That is the only thing you type. With an empty app ID the game doesn't use
Steam at all, which is how the engine comes.

## 4. Run it with Steam

Make sure the Steam client is running and signed in, then build and start the
game:

```bash
./assisi package dev
./assisi run game dev
```

**What success looks like:**

- The log says `Steam: running as <your Steam name> for app 480`.
- Press **Shift+Tab** in the game: the Steam overlay opens over it.
- Your friends list shows you playing the game (or "Spacewar", with 480).

If the log says something else instead, see [When it doesn't work](#when-it-doesnt-work).

The editor never starts Steam, so you can keep the editor open while the game
runs. Neither do headless runs and tests.

## 5. Use Steam from your game

Every system gets Steam as `ctx.steam`. You never need to check whether Steam
is there: without it (in the editor, or with Steam closed), every call just
reports that Steam is unavailable and does nothing.

### Achievements

First, add the achievement on the Steamworks site: open your game's page,
then **Stats & Achievements → Achievements**, add one, give it an **API name**
such as `ACH_FIRST_HUNT`, and **publish** the change. App 480 already has four
you can use: `ACH_WIN_ONE_GAME`, `ACH_WIN_100_GAMES`, `ACH_TRAVEL_FAR_ACCUM`
and `ACH_TRAVEL_FAR_SINGLE`.

Then unlock it from a system by its API name, and send it to Steam:

```cpp
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Steam/Steam.hpp>

/// Unlocks an achievement the moment a level starts.
ASYSTEM(Begin, name = "FirstAchievement")
void FirstAchievementSystem(Assisi::App::SystemContext &ctx);

void FirstAchievementSystem(Assisi::App::SystemContext &ctx)
{
    (void)ctx.steam->UnlockAchievement("ACH_WIN_ONE_GAME");
    (void)ctx.steam->StoreStats();
}
```

Steam shows the achievement pop-up once `StoreStats` sends it. Add
`FirstAchievement` to a level's systems, as in [Your first
system](first-system.md), and run the game. An achievement unlocks once per
account; after that, the call succeeds and nothing pops up.

### Everything else

| Call | What it does |
|---|---|
| `ctx.steam->PersonaName()` | The player's Steam name. |
| `ctx.steam->UserId()` | Their Steam account id. |
| `ctx.steam->IsOverlayActive()` | True while the overlay is open: pause your game. |
| `ctx.steam->SetStat("kills", 3)`, `StatInt("kills")` | Whole-number stats, which you define on the Steamworks site like achievements. `float` stats work the same. |
| `ctx.steam->SetRichPresence("status", "Hunting")` | What friends see you doing. |
| `ctx.steam->RunningOn()`, `SuggestedConfig()`, `IsRunningUnderProton()` | Which Steam hardware the game runs on, the settings preset Steam suggests for it, and whether it runs under Proton. |

Each call that can fail returns either its answer or the reason it couldn't,
so you can show the player something useful, e.g. when a stat name is wrong.

## 6. Releasing on Steam

A **ship** build of a game with an app ID is Steam-only:

- Started outside Steam, for example by double-clicking it, it asks Steam to
  start it and closes. Steam then starts it properly.
- If Steam can't start for it (Steam not signed in, or the account doesn't own
  the game), it refuses to run and says why in its log.

This stops casual copying, like someone zipping your game folder and passing it
on. It does **not** stop determined pirates; nothing does. Valve offers its own
extra protection, the Steam DRM wrapper, which you can turn on when you upload.

Debug and dev builds always run, Steam or not, so you can work without it.

**What a player gets**, from `./assisi release ship --version 1.0` (see
[Packaging your game](packaging.md)):

```text
Assisi-Game          the game
assets.pak           its content
libsteam_api.so      Steam's runtime library (steam_api64.dll on Windows)
```

Valve's agreement allows shipping that library inside your game.

**What never goes in git or to players:** the SDK, its zip, and the
`steamworks/` folder.

### `steam_appid.txt`: on your computer, never in a release

`steam_appid.txt` is a small file holding just your app ID. When it sits
beside the game, Steam takes the app ID from it instead of from the Steam
library, which is what lets you run the game straight from your build folder.
It's a development tool only: **Valve says never to ship it to players.**

- **Debug and dev builds** write it beside the game for you. You don't need to
  do anything.
- **Ship builds** don't get one, because a ship build is what players receive.
  `./assisi release` and `./assisi package` never include it either.
- **To try a ship build outside Steam on your own computer**, add one by hand:
  a file named `steam_appid.txt`, holding just your app ID, beside
  `Assisi-Game`. Without it, the game hands itself to Steam and closes. Delete
  the file when you're done testing.
- **If you ever copy a game folder by hand** to upload it, make sure
  `steam_appid.txt` isn't in it.

## When it doesn't work

- **`Steam: the Steam client is not running, or not as this user`**: start
  Steam and sign in, as the same user that runs the game. (Not as root, and not
  inside a container.)
- **`Steam would not start for this game: check its app id, and that this
  account owns it`**: check the number in `apps/game/CMakeLists.txt`. For your
  own app ID, the signed-in account must own the game; developers get it in
  their library once Valve sets the app up.
- **`the engine was built without the Steamworks SDK`**: the SDK isn't
  installed, or you built before installing it. Run step 2, then build again.
- **The game won't start at all and mentions `libsteam_api.so`**: the library
  isn't beside the game. Build again, or for a folder you copied by hand, copy
  `libsteam_api.so` from the build folder too.
- **A ship build closes as soon as it starts**: it asked Steam to start it,
  which is right for a player. To test it outside Steam, see [`steam_appid.txt`](#steam_appidtxt-on-your-computer-never-in-a-release).
- **A ship build won't configure, naming the Steamworks SDK**: a game with an
  app ID needs the SDK to build its release. Run step 2.

<details>
<summary>Why the SDK isn't part of the engine</summary>

Valve's Steamworks SDK Access Agreement lets a developer copy the SDK onto
their own computer to build their game, and ship the runtime library inside
that game. It doesn't let anyone redistribute the SDK itself, so the engine,
whose source is public, can't include it, download it for you, or keep it in
git. Everything else is in the engine: the code that uses Steam, the build
setup, and this page. The agreement is at
[partner.steamgames.com/documentation/sdk_access_agreement](https://partner.steamgames.com/documentation/sdk_access_agreement),
and Valve's own guide to the SDK at
[partner.steamgames.com/doc/sdk/api](https://partner.steamgames.com/doc/sdk/api).

</details>
