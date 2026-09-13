/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ImGuiQueries.hpp
/// @brief Small shared ImGui helpers used across the editor's translation units.

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Every query below null-checks the context first. It may not exist yet (before
// DebugUI initializes, or when the debug UI is disabled), and ImGui::GetIO()
// asserts without one.
inline bool ImGuiWantsMouse()
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
}

inline bool ImGuiWantsKeyboard()
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}

/// Ctrl or Shift held — the "add to the selection" modifier. One query for both
/// because HandleEntityPicking treats them the same.
inline bool ImGuiAdditiveModifier()
{
    return ImGui::GetCurrentContext() != nullptr && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
}

// --- Keyboard navigation of a search field's suggestion list -----------------
//
// A single-line InputText swallows Tab and the arrow keys, so its callbacks are
// the only place they surface. The callback records intent; the highlight moves
// afterwards, once the frame's match count is known — the field cannot know how
// many rows it is steering.

/// @brief What the field's keys asked for this frame.
struct ImGuiSuggestionNav
{
    int32_t move  = 0;     ///< -1 up, +1 down (Tab or the arrow keys).
    bool reset = false;    ///< Text was edited — snap back to the first row.
};

/// @brief Flags an InputText must carry for ImGuiSuggestionNavCallback to see
/// the keys, alongside whatever the caller adds.
inline constexpr ImGuiInputTextFlags kImGuiSuggestionNavFlags =
    ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory |
    ImGuiInputTextFlags_CallbackEdit;

/// @brief InputText callback taking an ImGuiSuggestionNav as its user data.
inline int ImGuiSuggestionNavCallback(ImGuiInputTextCallbackData *data)
{
    auto *nav = static_cast<ImGuiSuggestionNav *>(data->UserData);
    switch (data->EventFlag)
    {
    case ImGuiInputTextFlags_CallbackCompletion: nav->move = +1; break; // Tab
    case ImGuiInputTextFlags_CallbackHistory:
        nav->move = data->EventKey == ImGuiKey_UpArrow ? -1 : +1;
        break;
    case ImGuiInputTextFlags_CallbackEdit: nav->reset = true; break;
    default: break;
    }
    return 0;
}

/// @brief Where the highlight lands: an edit snaps it to the top, Tab and the
/// arrows step it with wrap-around across @p shown rows.
///
/// Clamps as well as steps, because @p shown shrinks as the query narrows and a
/// highlight left past the end would index a row that is no longer drawn.
inline int32_t ImGuiAdvanceSuggestion(int32_t current, const ImGuiSuggestionNav &nav, std::size_t shown)
{
    if (nav.reset || shown == 0)
        return 0;
    const int32_t count = static_cast<int32_t>(shown);
    if (nav.move != 0)
        current = (current + nav.move + count) % count;
    return std::clamp(current, 0, count - 1);
}
