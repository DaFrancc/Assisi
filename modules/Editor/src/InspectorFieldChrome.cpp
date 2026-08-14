/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file InspectorFieldChrome.cpp
/// @brief Implementation of the Inspector's per-field colour scoping.

#include <Assisi/Editor/InspectorFieldChrome.hpp>

#include <imgui.h>

namespace Assisi::Editor
{

/// AFIELD(norep): saved to disk like anything else, never sent. Dimmed rather
/// than disabled — it is authored data and the author *should* be editing it;
/// they only need to know it stays here.
///
/// A Hidden field is not drawn, so it is not tinted either. That is the ordering
/// the constructor exists to enforce.
ScopedFieldChrome::ScopedFieldChrome(RadioVisibility radio, bool norep)
    : _visible(radio != RadioVisibility::Hidden),
      _greyed(radio == RadioVisibility::Greyed),
      _tinted(_visible && norep)
{
    if (_tinted)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
}

ScopedFieldChrome::~ScopedFieldChrome()
{
    EndTint();
}

void ScopedFieldChrome::EndTint()
{
    if (_tinted)
    {
        ImGui::PopStyleColor();
        _tinted = false;
    }
}

} // namespace Assisi::Editor
