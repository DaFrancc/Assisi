/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ImGuiQueries.hpp
/// @brief Small shared ImGui helpers used across the sandbox's translation units.

#include <imgui.h>

// The ImGui context may not exist yet (before DebugUI initializes, or when the
// debug UI is disabled) — calling ImGui::GetIO() without a context asserts, so
// gate every query on GetCurrentContext() first.
inline bool ImGuiWantsMouse()
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

inline bool ImGuiWantsKeyboard()
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}

/// Ctrl or Shift held — the "add to the selection" modifier. One query for both
/// because the viewport treats them the same (see HandleEntityPicking).
inline bool ImGuiAdditiveModifier()
{
    return ImGui::GetCurrentContext() != nullptr && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
}
