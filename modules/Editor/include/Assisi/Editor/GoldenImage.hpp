/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GoldenImage.hpp
/// @brief A capture's picture against the one accepted for it on this GPU: what
/// counts as the same picture, and where accepted pictures are kept.
///
/// Accepted pictures are kept per GPU because drivers round differently, and
/// on the machine alone: they change whenever the look changes on purpose.

#include <Assisi/Render/FrameCapture.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Assisi::Editor
{

/// The largest difference in any channel, in 8-bit levels, two pixels can have
/// and still count as the same. Repeated runs differ by a level where automatic
/// exposure adapts over the frames' real durations, and drivers round the last
/// level their own way. Four let a whole picture brighten by 2% unnoticed.
inline constexpr std::uint32_t kGoldenChannelTolerance = 2u;

/// The share of a picture's pixels that may differ past the tolerance and still
/// pass: a few pixels where an edge lands exactly on a pixel centre flip
/// between drivers.
inline constexpr double kGoldenDifferingShare = 0.001;

/// @brief How a picture differs from the one it should be.
struct ImageDifference
{
    /// The expected picture dimmed, with each pixel that differs past the
    /// tolerance drawn red. Empty when the sizes differ.
    Render::CapturedImage highlight;
    /// Pixels that differ past the tolerance; every pixel when the sizes differ.
    std::uint64_t differingPixels = 0;
    /// The largest difference in any channel of any pixel, in 8-bit levels.
    std::uint32_t largestDifference = 0;
};

/// @brief How @p actual differs from @p expected, pixel by pixel.
[[nodiscard]] ImageDifference CompareImages(const Render::CapturedImage &actual,
                                            const Render::CapturedImage &expected);

/// @brief Whether @p difference is small enough for the two to be the same
/// picture: the same size, and few enough pixels past the tolerance.
[[nodiscard]] bool SamePicture(const ImageDifference &difference);

/// @brief The folder name for @p deviceName's pictures: its letters and digits,
/// with every other run of characters a single hyphen.
[[nodiscard]] std::string GoldenDeviceFolder(std::string_view deviceName);

/// @brief Where the picture accepted for the level at @p levelPath on
/// @p deviceName is kept under @p directory: <directory>/<device>/<level>.png.
[[nodiscard]] std::filesystem::path GoldenImagePath(const std::filesystem::path &directory,
                                                    std::string_view deviceName, std::string_view levelPath);

/// @brief Where the diff picture for @p imagePath goes: beside it, as
/// <name>.diff.png.
[[nodiscard]] std::filesystem::path DiffImagePath(const std::filesystem::path &imagePath);

/// @brief The picture in the PNG file at @p file, or nullopt if it cannot be
/// read.
[[nodiscard]] std::optional<Render::CapturedImage> ReadImage(const std::filesystem::path &file);

/// @brief Compare @p image with the golden picture at @p golden, writing a diff
/// picture to @p diffPath when they differ; or, when there is no golden
/// picture yet or @p accept, make @p image the golden one. Logs which.
///
/// @return Whether the run passes: the same picture, or one just accepted.
[[nodiscard]] bool CheckGoldenImage(const Render::CapturedImage &image, const std::filesystem::path &golden,
                                    const std::filesystem::path &diffPath, bool accept);

} // namespace Assisi::Editor
