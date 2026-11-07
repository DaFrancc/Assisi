/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestGoldenImage.cpp
/// @brief A capture's picture against its golden one: rounding passes, a
/// visible change fails and is drawn red, and the first picture or an accepted
/// one becomes the golden picture.

#include <doctest/doctest.h>

#include <Assisi/Editor/GoldenImage.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

using namespace Assisi;
using Assisi::Render::CapturedImage;

namespace
{

constexpr std::size_t kChannels = 4u;

/// A @p width x @p height picture of one grey.
CapturedImage Flat(std::uint32_t width, std::uint32_t height, std::uint8_t grey)
{
    CapturedImage image{.rgba = {}, .width = width, .height = height};
    image.rgba.assign(static_cast<std::size_t>(width) * height * kChannels, grey);
    return image;
}

std::uint8_t &Channel(CapturedImage &image, std::size_t pixel, std::size_t channel)
{
    return image.rgba[pixel * kChannels + channel];
}

/// A directory of its own under the system's temporary one, removed after.
struct ScratchDirectory
{
    std::filesystem::path path;

    explicit ScratchDirectory(const std::string &name)
        : path(std::filesystem::temp_directory_path() / ("assisi-golden-test-" + name))
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }

    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    ScratchDirectory(const ScratchDirectory &) = delete;
    ScratchDirectory &operator=(const ScratchDirectory &) = delete;
};

} // namespace

TEST_CASE("The same picture differs nowhere")
{
    const CapturedImage picture = Flat(8u, 4u, 120u);
    const Editor::ImageDifference difference = Editor::CompareImages(picture, picture);
    CHECK(difference.differingPixels == 0u);
    CHECK(difference.largestDifference == 0u);
    CHECK(Editor::SamePicture(difference));
}

TEST_CASE("Rounding passes, a visible change counts and is drawn red over the dimmed picture")
{
    const CapturedImage expected = Flat(4u, 1u, 120u);
    CapturedImage actual = expected;
    Channel(actual, 1u, 1u) = static_cast<std::uint8_t>(120u + Editor::kGoldenChannelTolerance);
    Channel(actual, 2u, 2u) = static_cast<std::uint8_t>(120u - Editor::kGoldenChannelTolerance - 1u);
    // Alpha is not part of what a window shows.
    Channel(actual, 3u, 3u) = 0u;

    const Editor::ImageDifference difference = Editor::CompareImages(actual, expected);
    CHECK(difference.differingPixels == 1u);
    CHECK(difference.largestDifference == Editor::kGoldenChannelTolerance + 1u);

    REQUIRE(difference.highlight.rgba.size() == expected.rgba.size());
    CapturedImage highlight = difference.highlight;
    CHECK(Channel(highlight, 2u, 0u) == 255u);
    CHECK(Channel(highlight, 2u, 1u) == 0u);
    CHECK(Channel(highlight, 2u, 2u) == 0u);
    CHECK(Channel(highlight, 1u, 0u) < 120u);
    CHECK(Channel(highlight, 1u, 0u) == Channel(highlight, 1u, 1u));
}

TEST_CASE("A few differing pixels pass and more fail")
{
    // One thousand pixels: the share allows exactly one.
    const CapturedImage expected = Flat(1000u, 1u, 60u);
    CapturedImage one = expected;
    Channel(one, 10u, 0u) = 200u;
    CHECK(Editor::SamePicture(Editor::CompareImages(one, expected)));

    CapturedImage two = one;
    Channel(two, 20u, 0u) = 200u;
    CHECK_FALSE(Editor::SamePicture(Editor::CompareImages(two, expected)));
}

TEST_CASE("Pictures of different sizes are never the same")
{
    const Editor::ImageDifference difference = Editor::CompareImages(Flat(4u, 4u, 9u), Flat(4u, 2u, 9u));
    CHECK(difference.differingPixels == 16u);
    CHECK(difference.highlight.rgba.empty());
    CHECK_FALSE(Editor::SamePicture(difference));
}

TEST_CASE("Each GPU keeps its pictures in a folder named after it")
{
    CHECK(Editor::GoldenDeviceFolder("NVIDIA GeForce RTX 3070") == "NVIDIA-GeForce-RTX-3070");
    CHECK(Editor::GoldenDeviceFolder("llvmpipe (LLVM 23.1.2, 256 bits)") == "llvmpipe-LLVM-23-1-2-256-bits");
    CHECK(Editor::GoldenDeviceFolder("  AMD  ") == "AMD");
    CHECK(Editor::GoldenDeviceFolder("") == "unknown-gpu");

    const std::filesystem::path root("golden");
    CHECK(Editor::GoldenImagePath(root, "AMD Radeon", "levels/BloomCheck.alvl") ==
          root / "AMD-Radeon" / "BloomCheck.png");
    CHECK(Editor::GoldenImagePath(root, "AMD Radeon", "") == root / "AMD-Radeon" / "untitled.png");
    CHECK(Editor::DiffImagePath("out/gpu/BloomCheck.png") == std::filesystem::path("out/gpu/BloomCheck.diff.png"));
}

TEST_CASE("The first picture is accepted, the same one passes, a changed one fails until accepted")
{
    const ScratchDirectory scratch("check");
    const std::filesystem::path golden = scratch.path / "GPU" / "Level.png";
    const std::filesystem::path diff = scratch.path / "Level.diff.png";
    const CapturedImage picture = Flat(16u, 16u, 100u);

    CHECK(Editor::CheckGoldenImage(picture, golden, diff, false));
    CHECK(std::filesystem::exists(golden));
    const std::optional<CapturedImage> stored = Editor::ReadImage(golden);
    REQUIRE(stored.has_value());
    CHECK(stored->width == 16u);
    CHECK(stored->rgba == picture.rgba);

    CHECK(Editor::CheckGoldenImage(picture, golden, diff, false));
    CHECK_FALSE(std::filesystem::exists(diff));

    const CapturedImage changed = Flat(16u, 16u, 140u);
    CHECK_FALSE(Editor::CheckGoldenImage(changed, golden, diff, false));
    CHECK(std::filesystem::exists(diff));

    CHECK(Editor::CheckGoldenImage(changed, golden, diff, true));
    CHECK(Editor::CheckGoldenImage(changed, golden, diff, false));
}
