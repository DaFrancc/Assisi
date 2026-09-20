/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenReader.hpp
/// @brief Where a screen's bytes come from, installed per executable.
///
/// A shipped game reads the blobs the cook wrote into its package; the editor
/// compiles a source file in process and reads the blob it just produced. Both
/// answer the same question and both end in ReadCookedScreen, so there is one
/// path from bytes to a document and no second one that reads markup.
///
/// That is the difference between this seam and the font's. A font has two
/// readers because the editor rasterises from the source; a screen has two ways
/// of *getting bytes* and one way of reading them.

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <string_view>

namespace Assisi::Core
{
class AssetProvider;
}

namespace Assisi::Mondrian
{

/// @brief Why a screen's bytes did not arrive, or did not read.
enum class ScreenReadError : uint8_t
{
    Missing,    ///< No screen at that path.
    Unreadable, ///< It exists and its bytes could not be read.
    Invalid,    ///< The bytes were read and are not a usable screen.
    NoReader,   ///< No reader is installed.
    Count
};

[[nodiscard]] std::string_view ToString(ScreenReadError error) noexcept;

using ScreenReader = std::function<std::expected<ScreenDocument, ScreenReadError>(std::string_view vpath)>;

/// @brief Install the reader every screen load goes through, returning the one
/// it replaces. Install at startup, before the first screen loads: the reader
/// is read without a lock.
ScreenReader SetScreenReader(ScreenReader reader);

/// @brief The screen at @p vpath, through the installed reader.
[[nodiscard]] std::expected<ScreenDocument, ScreenReadError> LoadScreenDocument(std::string_view vpath);

/// @brief The reader for screens cooked into @p provider.
[[nodiscard]] std::expected<ScreenDocument, ScreenReadError> ReadCookedScreen(const Core::AssetProvider &provider,
                                                                              std::string_view vpath);

} // namespace Assisi::Mondrian
