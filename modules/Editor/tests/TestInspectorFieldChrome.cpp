/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestInspectorFieldChrome.cpp
/// @brief An Inspector field that is not drawn leaves the ImGui colour stack
/// where it found it.
///
/// The Inspector dims an AFIELD(norep) field with a colour push and pops it once
/// the field has been drawn. A field that is *also* an AFIELD(radio) listener at
/// an inactive value is not drawn at all, and the skip used to happen after the
/// push — one stack entry leaked per such field per frame, which drew every
/// later widget in the disabled colour and tripped a debug ImGui build's
/// end-of-frame balance assert.
///
/// ScopedFieldChrome takes the visibility verdict as a constructor argument for
/// exactly that reason: there is no way to push the tint without first deciding
/// whether the field is drawn.
///
/// These need an ImGui context but not a frame or a backend — PushStyleColor and
/// PopStyleColor touch the context's colour stack and nothing else.

#include <doctest/doctest.h>

#include <Assisi/Editor/InspectorFieldChrome.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>

using Assisi::Editor::RadioVisibility;
using Assisi::Editor::ScopedFieldChrome;

namespace
{

/// The depth ImGui itself reports. The defect this covers is one entry per frame,
/// so the assertion is equality, not a bound.
int32_t ColourStackDepth()
{
    return static_cast<int32_t>(ImGui::GetCurrentContext()->ColorStack.Size);
}

/// A bare context: enough for the colour stack, nothing drawn.
struct ImGuiContextFixture
{
    ImGuiContextFixture() { ImGui::CreateContext(); }
    ~ImGuiContextFixture() { ImGui::DestroyContext(); }

    ImGuiContextFixture(const ImGuiContextFixture &)            = delete;
    ImGuiContextFixture &operator=(const ImGuiContextFixture &) = delete;
};

} // namespace

TEST_CASE("A hidden norep field pushes nothing on the colour stack")
{
    const ImGuiContextFixture context;
    const int32_t baseline = ColourStackDepth();

    {
        const ScopedFieldChrome chrome{RadioVisibility::Hidden, /*norep=*/ true};
        CHECK_FALSE(chrome.Visible());
        // The caller `continue`s here. Nothing may be outstanding at this point,
        // because nothing unwinds a `continue`.
        CHECK(ColourStackDepth() == baseline);
    }

    CHECK(ColourStackDepth() == baseline);
}

TEST_CASE("A drawn norep field is tinted, and the tint is balanced")
{
    const ImGuiContextFixture context;
    const int32_t baseline = ColourStackDepth();

    {
        const ScopedFieldChrome chrome{RadioVisibility::Active, /*norep=*/ true};
        CHECK(chrome.Visible());
        CHECK_FALSE(chrome.Greyed());
        CHECK(ColourStackDepth() == baseline + 1);
    }

    CHECK(ColourStackDepth() == baseline);
}

TEST_CASE("A greyed norep listener is drawn, and tinted like any other")
{
    const ImGuiContextFixture context;
    const int32_t baseline = ColourStackDepth();

    {
        const ScopedFieldChrome chrome{RadioVisibility::Greyed, /*norep=*/ true};
        CHECK(chrome.Visible());
        CHECK(chrome.Greyed());
        CHECK(ColourStackDepth() == baseline + 1);
    }

    CHECK(ColourStackDepth() == baseline);
}

TEST_CASE("A field that is not norep is never tinted")
{
    const ImGuiContextFixture context;
    const int32_t baseline = ColourStackDepth();

    for (const RadioVisibility radio :
         {RadioVisibility::Active, RadioVisibility::Greyed, RadioVisibility::Hidden})
    {
        const ScopedFieldChrome chrome{radio, /*norep=*/ false};
        CHECK(ColourStackDepth() == baseline);
    }

    CHECK(ColourStackDepth() == baseline);
}

TEST_CASE("EndTint pops once, and the destructor does not pop it again")
{
    const ImGuiContextFixture context;
    const int32_t baseline = ColourStackDepth();

    {
        ScopedFieldChrome chrome{RadioVisibility::Active, /*norep=*/ true};
        REQUIRE(ColourStackDepth() == baseline + 1);

        // What the call site does before the trailing "(server-only)" tag.
        chrome.EndTint();
        CHECK(ColourStackDepth() == baseline);

        // Idempotent: a second call must not eat an entry someone else pushed.
        chrome.EndTint();
        CHECK(ColourStackDepth() == baseline);
    }

    CHECK(ColourStackDepth() == baseline);
}

TEST_CASE("A tint left outstanding is popped by the destructor")
{
    const ImGuiContextFixture context;
    const int32_t baseline = ColourStackDepth();

    {
        // The call site's early-return paths never reach EndTint; the guard is
        // what keeps them balanced.
        const ScopedFieldChrome chrome{RadioVisibility::Active, /*norep=*/ true};
        REQUIRE(ColourStackDepth() == baseline + 1);
    }

    CHECK(ColourStackDepth() == baseline);
}
