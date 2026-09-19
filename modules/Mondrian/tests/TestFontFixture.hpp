/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestFontFixture.hpp
/// @brief A small font whose metrics are whole pixels, so layout tests can
/// state exact positions: 'A' advances 20, a space 8, and a line is 39 tall.

#include <Assisi/Mondrian/Font.hpp>

#include <cstddef>
#include <cstdint>

namespace Assisi::Mondrian::Testing
{

inline constexpr float kFixtureSize = 32.f;
inline constexpr float kFixtureLineHeight = 39.f;
inline constexpr float kFixtureAdvanceA = 20.f;
inline constexpr float kFixtureAdvanceSpace = 8.f;

inline Font FixtureFont()
{
    constexpr uint32_t kAtlasSide = 32;
    Font font;
    font.pixelSize = kFixtureSize;
    font.ascender = 29.f;
    font.descender = -7.f;
    font.lineHeight = kFixtureLineHeight;
    font.atlasWidth = kAtlasSide;
    font.atlasHeight = kAtlasSide;
    font.atlas.assign(static_cast<std::size_t>(kAtlasSide) * kAtlasSide, 0);
    font.glyphs = {
        Glyph{.advance = 24.f, .index = 0, .x = 0, .y = 0, .width = 10, .height = 12, .bearingX = 1, .bearingY = 23},
        Glyph{.advance = kFixtureAdvanceA,
              .index = 10,
              .x = 10,
              .y = 0,
              .width = 8,
              .height = 10,
              .bearingX = 1,
              .bearingY = 23},
        Glyph{.advance = kFixtureAdvanceSpace, .index = 40},
    };
    font.cmap = {CmapEntry{.codepoint = ' ', .glyph = 40}, CmapEntry{.codepoint = 'A', .glyph = 10}};
    return font;
}

} // namespace Assisi::Mondrian::Testing
