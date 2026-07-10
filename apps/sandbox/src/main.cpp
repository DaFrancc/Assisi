/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
/// @file main.cpp
/// @brief Assisi Sandbox entry point. The app itself lives in SandboxApp.* —
/// see SandboxApp.hpp for the translation-unit split.

#include "SandboxApp.hpp"

#include <cstdlib>

int main()
{
    SandboxApp app;
    if (!app.Initialize())
    {
        return EXIT_FAILURE;
    }
    app.Run();
    return EXIT_SUCCESS;
}
