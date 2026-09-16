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
#include <Assisi/Runtime/SceneSerializer.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace
{
constexpr const char *kUsage =
    "Usage: Assisi-Game [options]\n"
    "  --headless              run with no window, renderer or input — just the\n"
    "                          fixed-step simulation\n"
    "  --ticks <n>             stop after n fixed ticks (0 = run until the\n"
    "                          player quits, the default)\n"
    "  --verbosity <level>     lowest level to log: trace, debug, info, warn,\n"
    "                          error, fatal\n"
    "  -h, --help              show this help and exit\n";

/// What argv resolved to, plus whether main should stop before starting.
struct GameArgs
{
    Assisi::App::GameLaunch launch;
    bool headless   = false;
    bool shouldExit = false;
};

/// Parses argv into @p out. Returns false with a message printed when the
/// arguments are malformed; sets shouldExit when --help was handled, which is a
/// clean early exit rather than an error.
bool ParseArgs(int32_t argc, char **argv, GameArgs &out)
{
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
    // Levels and blueprints are read from the source tree until the game reads
    // its pak.
    (void)Assisi::Runtime::SceneSerializer::SetDocumentReader(&Assisi::Runtime::SceneSerializer::ReadTextDocument);

    GameArgs args;
    if (!ParseArgs(static_cast<int32_t>(argc), argv, args))
    {
        return EXIT_FAILURE;
    }
    if (args.shouldExit)
    {
        return EXIT_SUCCESS;
    }

    Assisi::App::GameApp app(args.launch);
    app.SetHeadless(args.headless);
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();

    // A game that refused to start is not a game that finished. Only main() can
    // say so to whatever launched it, and a launcher reads the exit code.
    return app.StartupFailed() ? EXIT_FAILURE : EXIT_SUCCESS;
}
