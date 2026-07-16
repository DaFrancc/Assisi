/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file main.cpp
/// @brief Assisi Sandbox entry point. The app itself lives in SandboxApp.* —
/// see SandboxApp.hpp for the translation-unit split.

#include "SandboxApp.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
constexpr const char *kUsage =
    "Usage: Assisi-Sandbox [options]\n"
    "  -l, --load-level <lvl>  virtual path of a level to open at startup,\n"
    "                          e.g. levels/Materials.alvl\n"
    "  -h, --help              show this help and exit\n";

// Parses argv into a startup-level virtual path (empty if none). Returns false
// with a message printed when the arguments are malformed; sets shouldExit when
// --help was handled (a clean early exit, not an error).
bool ParseArgs(int argc, char **argv, std::string_view &startupLevel, bool &shouldExit)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            std::fputs(kUsage, stdout);
            shouldExit = true;
            return true;
        }
        if (arg == "-l" || arg == "--load-level")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "%.*s requires a level path\n\n%s", static_cast<int>(arg.size()), arg.data(),
                             kUsage);
                return false;
            }
            startupLevel = argv[++i];
        }
        else
        {
            std::fprintf(stderr, "Unknown argument '%.*s'\n\n%s", static_cast<int>(arg.size()), arg.data(), kUsage);
            return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    std::string_view startupLevel;
    bool             shouldExit = false;
    if (!ParseArgs(argc, argv, startupLevel, shouldExit))
    {
        return EXIT_FAILURE;
    }
    if (shouldExit)
    {
        return EXIT_SUCCESS;
    }

    SandboxApp app;
    if (!startupLevel.empty())
    {
        app.SetStartupLevel(startupLevel);
    }
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}
