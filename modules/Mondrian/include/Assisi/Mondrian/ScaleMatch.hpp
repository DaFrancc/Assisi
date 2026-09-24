/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScaleMatch.hpp
/// @brief Which side of the screen a UI pixel is measured against.
///
/// Its own header so a game's settings can name it without taking in layout.

#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>

namespace Assisi::Mondrian
{

/// @brief Which side of the viewport the 1920×1080 reference is matched against
/// to decide how large a UI pixel is.
AENUM()
enum class ScaleMatch : std::uint8_t
{
    /// A UI pixel is 1/1080 of the viewport's shorter side. Readable on any
    /// shape: a portrait phone scales as a landscape monitor of the same height
    /// does, and wider screens gain space rather than shrinking what is on them.
    ShorterSide,

    /// A UI pixel is 1/1920 of the viewport's width, for a layout designed across.
    Width,

    /// A UI pixel is 1/1080 of the viewport's height, for a layout designed down.
    Height,

    Count
};

} // namespace Assisi::Mondrian
