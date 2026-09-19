/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file FontDescription.hpp
/// @brief What to cook a font from: the font file, which characters, and at what size.
///
/// A `.afont` document. The choices here are per use rather than per font file,
/// so they live in data: a language that needs more characters adds a range,
/// with no code change.

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::Mondrian
{

AASSET()
struct FontDescription
{
    /// Inclusive codepoint ranges, flattened: first, last, first, last.
    AFIELD() std::vector<uint32_t> ranges;

    /// The font file, as a virtual path.
    AFIELD() Assisi::Core::AssetPath source;

    /// The size glyphs are rasterised at, in pixels. A distance field draws
    /// crisply well above and below it.
    AFIELD() float pixelSize = 48.f;

    /// How far the distance field reaches from the outline, in texels.
    AFIELD() uint32_t spread = 8;
};

} // namespace Assisi::Mondrian
