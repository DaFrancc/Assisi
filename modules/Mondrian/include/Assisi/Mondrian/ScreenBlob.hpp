/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenBlob.hpp
/// @brief A screen document's cooked form: the bytes a shipped game reads.
///
/// The writer and the reader live together so the layout has one definition.
/// The cook writes through WriteCookedScreen and every load reads through
/// ReadCookedScreen, in the game and in the editor alike — there is no second
/// path that reads markup, which is what keeps a parser out of the game.
///
/// Reading is framing plus consistency. Every count and length is checked
/// against the bytes left before anything is allocated, and the tree the table
/// describes is checked before it is handed back: a parent that points forward
/// or past the end is refused here rather than walked by the loader.

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <Assisi/Core/BitStream.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace Assisi::Mondrian
{

/// @brief Version of the screen payload's layout, separate from the blob
/// envelope's.
///
/// Part of the screen cooker's cache key, so bumping it re-cooks every screen.
inline constexpr uint8_t kScreenPayloadVersion = 2;

/// @brief Why bytes did not read as a cooked screen.
enum class CookedScreenError : uint8_t
{
    NotAScreen,         ///< Not a cooked blob, or a blob of another kind.
    Truncated,          ///< The bytes ran out inside the payload.
    UnsupportedVersion, ///< A screen layout this build does not read.
    Invalid,            ///< Framed correctly and describing no usable screen.
    Count
};

/// @brief A short human-readable description, for a load failure's log line.
[[nodiscard]] std::string_view ToString(CookedScreenError error) noexcept;

/// @brief Write @p document as a complete cooked blob, envelope included.
void WriteCookedScreen(Core::BitWriter &writer, const ScreenDocument &document);

/// @brief Read a blob written by WriteCookedScreen.
///
/// Every length is checked against the bytes left before anything is
/// allocated. An empty table, a parent index at or past its own node, a focus
/// index naming no node, or an enumerator this build does not have is Invalid.
[[nodiscard]] std::expected<ScreenDocument, CookedScreenError> ReadCookedScreen(std::span<const std::byte> bytes);

} // namespace Assisi::Mondrian
