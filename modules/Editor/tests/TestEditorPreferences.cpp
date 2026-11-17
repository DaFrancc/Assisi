/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestEditorPreferences.cpp
/// @brief editor.json: what is written reads back the same, and a file that is
///        missing settings, holds the wrong kind of value or is not JSON at all
///        still gives the editor something to start with.

#include <doctest/doctest.h>

#include <Assisi/Editor/EditorPreferences.hpp>

using Assisi::Editor::EditorPreferences;
using Assisi::Editor::FormatEditorPreferences;
using Assisi::Editor::kMaxSplineThickness;
using Assisi::Editor::kMinSplineThickness;
using Assisi::Editor::ParseEditorPreferences;

TEST_CASE("EditorPreferences: what is written reads back the same")
{
    EditorPreferences chosen;
    chosen.splines.color = glm::vec4(0.25f, 0.5f, 0.75f, 1.f);
    chosen.splines.thickness = 4.5f;

    CHECK(ParseEditorPreferences(FormatEditorPreferences(chosen)) == chosen);
}

TEST_CASE("EditorPreferences: a file that is not JSON, or says nothing, gives the defaults")
{
    CHECK(ParseEditorPreferences("") == EditorPreferences{});
    CHECK(ParseEditorPreferences("{ not json") == EditorPreferences{});
    CHECK(ParseEditorPreferences("[1, 2, 3]") == EditorPreferences{});
    CHECK(ParseEditorPreferences("{}") == EditorPreferences{});
}

TEST_CASE("EditorPreferences: a missing setting keeps its default and the rest are read")
{
    const EditorPreferences read = ParseEditorPreferences(R"({"splines": {"thickness": 3}})");
    CHECK(read.splines.thickness == doctest::Approx(3.f));
    CHECK(read.splines.color == EditorPreferences{}.splines.color);
}

TEST_CASE("EditorPreferences: a value of the wrong kind keeps its default")
{
    const EditorPreferences read =
        ParseEditorPreferences(R"({"splines": {"color": [1, 0, "red", 1], "thickness": "wide"}})");
    CHECK(read == EditorPreferences{});
}

TEST_CASE("EditorPreferences: a value out of range is brought into it")
{
    const EditorPreferences read =
        ParseEditorPreferences(R"({"splines": {"color": [2, -1, 0.5, 1], "thickness": 100}})");
    CHECK(read.splines.color == glm::vec4(1.f, 0.f, 0.5f, 1.f));
    CHECK(read.splines.thickness == doctest::Approx(kMaxSplineThickness));
    CHECK(ParseEditorPreferences(R"({"splines": {"thickness": 0}})").splines.thickness ==
          doctest::Approx(kMinSplineThickness));
}
