# The build tool

Everything you do with the engine besides writing code goes through one
command, `assisi`, in the top folder of the repository: building, starting the
editor, running tests, packaging a game. On Linux you type `./assisi`; on
Windows, in the Developer Command Prompt, just `assisi`. This book writes
`./assisi`.

It works two ways:

- **With a command**, it does that and nothing else: `./assisi build`,
  `./assisi package ship`. This is how you use it in scripts, too.
- **With nothing after it**, in a terminal, it opens a menu. More on that
  [below](#the-menu).

## The commands

Run `./assisi --help` for the list, or `./assisi <command> --help` for one
command's options.

| Command | What it does |
|---|---|
| `./assisi build` | Builds the editor, the tests and the shaders. Add `--game` to also build the game without the editor. |
| `./assisi run editor` | Builds, then starts the editor. Anything after `--` goes to the editor: `./assisi run editor -- -l levels/Test.alvl`. |
| `./assisi run game` | Builds, then starts the packaged game. Package it first. |
| `./assisi test` | Builds, then runs the tests. Anything after `--` goes to ctest: `./assisi test -- -R Audio`. |
| `./assisi package ship` | Builds the game, cooks the assets and packs them: the folder a player gets. See [Packaging your game](packaging.md). |
| `./assisi release ship --version 1.0` | Packages, then keeps the game and its pak in `releases/1.0/` so later packages keep updates small. See [Packaging your game](packaging.md#releasing-and-keeping-updates-small). |
| `./assisi cook`, `./assisi pack` | The two halves of packaging on their own. |
| `./assisi clean dev` | Deletes a build's folder, so the next build starts from nothing. `--all` deletes every build. |
| `./assisi doctor` | Checks you have every tool the build needs, and says what to install if not. |
| `./assisi format` | Formats the C++ code. `--check` only reports. |

Most commands take a **level**: `debug`, `dev` or `ship`. Leave it off and you
get your default level, which starts out as dev. The **compiler** is separate:
add `--compiler clang` (or `gcc`, or `msvc` on Windows) to choose one, or leave
it off for your default compiler, which starts out as GCC on Linux and MSVC on
Windows. Change either default with `./assisi default ship` or `./assisi default
--compiler clang`; they're yours alone and never go into git. `package` and
`clean` always want the level named, so you never package or delete the wrong
one by accident.

A few flags pick special builds: `--profiler` compiles in the profiler,
`--sanitize address` or `--sanitize thread` makes a debug build that hunts
memory bugs or data races, and `--steam-runtime` makes a ship build for every
Linux distribution. [Build types](build-types.md) explains all of them.

The first time you build something, the tool configures it for you: CMake picks
the compiler and downloads the engine's libraries. You never have to configure
by hand, but `./assisi configure` is there if you want to do it again.

## History and recipes

The tool remembers what you run. `./assisi history` lists it, newest first, and
whether each one worked. `./assisi again` runs the last one again;
`./assisi again 3` runs the third.

A command you run often can be saved as a **recipe** and run by its name:

```bash
./assisi save ship-it -- package ship
./assisi ship-it
```

`./assisi save ship-it` with nothing after it saves the last command you ran.
`./assisi recipes` lists your recipes, and `./assisi forget ship-it` deletes
one. Like your history, they're kept in `.assisi/` in the top folder, which
never goes into git.

History and recipes always write the level and compiler out in full, like
`./assisi package ship --compiler gcc`, so changing your defaults later never
changes what an old entry does.

A recipe isn't the same thing as a CMake **preset**: a preset is one build
folder's settings in `CMakePresets.json`, like `gcc-dev`, and a recipe is a
whole `assisi` command you named.

## The menu

`./assisi` on its own opens a menu in the terminal. On the right is every build,
by level, compiler and extras, with a dot for each thing that exists of it:
configured, editor built, game built, assets cooked, assets packed. Your default
level and compiler are highlighted. On the
left are your recipes and your most used commands.

The bottom line always shows the exact `./assisi` command for whatever is
selected, so the menu teaches you the commands as you use it.

| Key | Does |
|---|---|
| **↑ ↓**, **Tab** | Move around |
| **Enter** | Run the selected recipe or command; on a build, open its build menu |
| **b**, **k**, **p**, **v** | On a build: open the build, cook, package or release menu |
| **e**, **r**, **t** | On a build: open the menu to start its editor, start its game, or test it |
| **D** | On a build: make its level and compiler your defaults |
| **1**–**9** | Run that entry of your most used commands |
| **a** | Run the last command again |
| **s** | Save the selected command, or the one that just ran, as a recipe |
| **/** | Filter the lists by typing |
| **Ctrl+P** | Search every command and recipe |
| **?** | Show every key |
| **q** | Quit |

The keys on a build never start anything by themselves. Each opens a menu
holding every setting of that action, already set to the build you were on:
the package menu, for example, has the level, compiler, Steam Runtime switch
and which earlier release to match. Change what you need and choose **Run**
(or press **Ctrl+R**); **Ctrl+S** saves it as a recipe instead, and **Esc**
goes back without running anything. A recipe or a most-used command, on the
other hand, runs as soon as you choose it, because you already picked every
setting when you first ran it.

When something runs, the menu steps aside and the build prints straight to your
terminal as usual. When it finishes, press **Enter** to go back.

The menu is built with [Textual](https://textual.textualize.io/). The first
time you open it, the tool downloads Textual and the few packages it needs into
`out/tool-env/`, which needs an internet connection and Python 3.10 or newer.
It never touches the rest of your system. If that doesn't work, the tool says
why, and every command still works without the menu. `./assisi tui --reinstall`
downloads it again.
