/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file GameMain.cpp
/// @brief The game's entry point — the binary a player runs.
///
/// Its whole job is to hand App::GameApp a launch and run it. There is no
/// editor in this link and no way to ask for one: the shape of the target is
/// what excludes it, not a flag it declines to parse.
///
/// The verbs here are deliberately few. A shipped game takes no level path — it
/// boots what it ships with — so the two below exist for the build machine
/// rather than the player: a windowed run ends when someone closes the window,
/// and nothing in CI is going to.

#include "LaunchArgs.hpp"

#include <Assisi/App/GameApp.hpp>
#include <Assisi/Core/Logger.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32
/// The game is a GUI-subsystem program on Windows, so it opens no console
/// window, and launched from a terminal it has no stdout either: --help and a
/// headless run would print nothing. Borrow the terminal's console for any
/// stream that was not handed a real handle. A redirect or a pipe (CTest, a
/// build step) already was, and is left alone.
void AttachParentConsole()
{
    const bool needOut = GetStdHandle(STD_OUTPUT_HANDLE) == nullptr;
    const bool needErr = GetStdHandle(STD_ERROR_HANDLE) == nullptr;
    if ((!needOut && !needErr) || !AttachConsole(ATTACH_PARENT_PROCESS))
    {
        return;
    }
    auto reopen = [](FILE *stream, DWORD stdHandle)
                  {
                      FILE *reopened = nullptr;
                      if (freopen_s(&reopened, "CONOUT$", "w", stream) == 0)
                      {
                          // Published as the process's handle too, so Core::HasConsoleOutput()
                          // sees the console and the logger adds its console sink.
                          SetStdHandle(stdHandle, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream))));
                      }
                  };
    if (needOut)
    {
        reopen(stdout, STD_OUTPUT_HANDLE);
    }
    if (needErr)
    {
        reopen(stderr, STD_ERROR_HANDLE);
    }
}
#endif

constexpr const char *kUsage =
    "Usage: Assisi-Game [options]\n"
    "  --headless              run with no window, renderer or input - just the\n"
    "                          fixed-step simulation\n"
    "  --ticks <n>             stop after n fixed ticks (0 = run until the\n"
    "                          player quits, the default)\n"
    "  --verbosity <level>     lowest level to log: trace, debug, info, warn,\n"
    "                          error, fatal\n"
#ifdef ASSISI_PAK_OVERRIDES
    "  --pak <path>            read content from this package instead of the\n"
    "                          one beside the executable (also ASSISI_PAK)\n"
#endif
#if defined(ASSISI_CHIARA_ENABLED)
    "  --benchmark [level]     fly the level's camera paths uncapped with 8x MSAA,\n"
    "                          FXAA and Ultra shadows, record a Chiara session,\n"
    "                          then exit. Without a level, flies the startup scene\n"
    "  --benchmark-seconds <n> how long the flight takes (default 15)\n"
    "  --benchmark-passes      also time each render pass. Splits render\n"
    "                          passes, so the frame total is not the shipped one\n"
    "  --benchmark-shots <dir> instead of measuring, stop at six points along the\n"
    "                          route and write a PNG of each into dir\n"
#endif
    "  -h, --help              show this help and exit\n";

#ifdef ASSISI_PAK_OVERRIDES
/// The environment variable naming a package to read, below --pak.
constexpr const char *kPakEnvironment = "ASSISI_PAK";
#endif

/// What argv resolved to, plus whether main should stop before starting.
struct GameArgs
{
    Assisi::App::GameLaunch launch;
    bool headless   = false;
    bool shouldExit = false;
};

#if defined(ASSISI_CHIARA_ENABLED)
/// The benchmark settings in @p args, created by whichever benchmark flag comes
/// first, so the flags can be given in any order.
Assisi::App::GameBenchmark &BenchmarkOf(GameArgs &args)
{
    if (!args.launch.benchmark)
    {
        args.launch.benchmark.emplace();
    }
    return *args.launch.benchmark;
}
#endif

/// Parses argv into @p out. Returns false with a message printed when the
/// arguments are malformed; sets shouldExit when --help was handled, which is a
/// clean early exit rather than an error.
bool ParseArgs(int32_t argc, char **argv, GameArgs &out)
{
#ifdef ASSISI_PAK_OVERRIDES
    // Development only: a shipped build reads the package beside it and nothing
    // else, so a player cannot point the game at content that was never shipped.
    if (const char *pak = std::getenv(kPakEnvironment); pak != nullptr && *pak != '\0')
    {
        out.launch.pak = pak;
    }
#endif

    for (int32_t i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            std::fputs(kUsage, stdout);
            out.shouldExit = true;
            return true;
        }

        if (arg == "--headless")
        {
            out.headless = true;
        }
        else if (arg == "--ticks")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--ticks requires a tick count\n\n%s", kUsage);
                return false;
            }
            const std::string_view value = argv[++i];
            int32_t ticks = 0;
            if (!Game::ParsePositive(value, ticks))
            {
                std::fprintf(stderr, "--ticks expects a positive integer, got '%.*s'\n\n%s",
                             static_cast<int>(value.size()), value.data(), kUsage);
                return false;
            }
            out.launch.tickLimit = static_cast<std::uint64_t>(ticks);
        }
#if defined(ASSISI_CHIARA_ENABLED)
        else if (arg == "--benchmark")
        {
            Assisi::App::GameBenchmark &benchmark = BenchmarkOf(out);
            // The level is optional: a next argument that is another flag is
            // not one.
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                benchmark.level = argv[++i];
            }
        }
        else if (arg == "--benchmark-seconds")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--benchmark-seconds requires a number of seconds\n\n%s", kUsage);
                return false;
            }
            const std::string_view value = argv[++i];
            int32_t seconds = 0;
            if (!Game::ParsePositive(value, seconds))
            {
                std::fprintf(stderr, "--benchmark-seconds expects a positive integer, got '%.*s'\n\n%s",
                             static_cast<int>(value.size()), value.data(), kUsage);
                return false;
            }
            BenchmarkOf(out).seconds = static_cast<double>(seconds);
        }
        else if (arg == "--benchmark-passes")
        {
            BenchmarkOf(out).passTiming = true;
        }
        else if (arg == "--benchmark-shots")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--benchmark-shots requires a directory\n\n%s", kUsage);
                return false;
            }
            BenchmarkOf(out).shotsDirectory = argv[++i];
        }
#endif
#ifdef ASSISI_PAK_OVERRIDES
        else if (arg == "--pak")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--pak requires a path\n\n%s", kUsage);
                return false;
            }
            out.launch.pak = argv[++i];
        }
#endif
        else if (arg == "--verbosity")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--verbosity requires a level name\n\n%s", kUsage);
                return false;
            }
            const std::string_view value = argv[++i];
            const std::optional<Assisi::Core::LogLevel> level = Assisi::Core::ParseLogLevel(value);
            if (!level)
            {
                std::fprintf(stderr, "--verbosity: '%.*s' is not a level name. Valid names are:",
                             static_cast<int>(value.size()), value.data());
                for (const std::string_view name : Assisi::Core::LogLevelNames())
                {
                    std::fprintf(stderr, " %.*s", static_cast<int>(name.size()), name.data());
                }
                std::fprintf(stderr, "\n\n%s", kUsage);
                return false;
            }
            // Applied here rather than stored: this runs before the Application
            // exists, so it takes effect for every line the engine emits,
            // including the ones from bring-up.
            Assisi::Core::GetLogger().SetMinLevel(*level);
        }
        else
        {
            std::fprintf(stderr, "Unknown argument '%.*s'\n\n%s", static_cast<int>(arg.size()), arg.data(),
                         kUsage);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    AttachParentConsole();
#endif

    // No readers are installed here: everything a game reads comes from its
    // content package, and GameApp installs the readers over it once it is open.
    GameArgs args;
    if (!ParseArgs(static_cast<int32_t>(argc), argv, args))
    {
        return EXIT_FAILURE;
    }
    if (args.shouldExit)
    {
        return EXIT_SUCCESS;
    }
    if (args.headless && args.launch.benchmark)
    {
        std::fprintf(stderr, "--benchmark renders, so it cannot run --headless\n\n%s", kUsage);
        return EXIT_FAILURE;
    }

    Assisi::App::GameApp app(args.launch);
    app.SetHeadless(args.headless);
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
}
