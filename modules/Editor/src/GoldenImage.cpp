/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/GoldenImage.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Image/Decode.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <system_error>
#include <utility>

namespace Assisi::Editor
{
namespace
{

/// A captured pixel's channels: red, green, blue and alpha.
constexpr std::size_t kChannels = 4u;
/// The channels compared. Alpha is left out: what a window shows ignores it.
constexpr std::size_t kColorChannels = 3u;
/// Where red sits in a pixel.
constexpr std::size_t kRedChannel = 0u;
/// How much darker than itself the expected picture shows under the
/// highlight, so red stands out against any scene.
constexpr std::uint32_t kHighlightDimming = 3u;
constexpr std::uint8_t kFullChannel = 255u;
/// The folder for a device that reports no name.
constexpr std::string_view kUnnamedDevice = "unknown-gpu";
/// The picture's name when the editor started with no level.
constexpr std::string_view kUnnamedLevel = "untitled";
constexpr std::string_view kImageExtension = ".png";
constexpr std::string_view kDiffSuffix = ".diff.png";

bool IsNameCharacter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
}

std::size_t PixelCount(const Render::CapturedImage &image)
{
    return static_cast<std::size_t>(image.width) * image.height;
}

bool WellFormed(const Render::CapturedImage &image)
{
    return image.rgba.size() == PixelCount(image) * kChannels;
}

} // namespace

ImageDifference CompareImages(const Render::CapturedImage &actual, const Render::CapturedImage &expected)
{
    ImageDifference difference;
    if (actual.width != expected.width || actual.height != expected.height || !WellFormed(actual) ||
        !WellFormed(expected))
    {
        difference.differingPixels = std::max(PixelCount(actual), PixelCount(expected));
        return difference;
    }

    const std::size_t pixels = PixelCount(expected);
    difference.highlight.width = expected.width;
    difference.highlight.height = expected.height;
    difference.highlight.rgba.resize(pixels * kChannels);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const std::size_t first = pixel * kChannels;
        std::uint32_t largest = 0u;
        for (std::size_t channel = 0; channel < kColorChannels; ++channel)
        {
            const int delta = static_cast<int>(actual.rgba[first + channel]) -
                              static_cast<int>(expected.rgba[first + channel]);
            largest = std::max(largest, static_cast<std::uint32_t>(std::abs(delta)));
        }
        difference.largestDifference = std::max(difference.largestDifference, largest);
        const bool differs = largest > kGoldenChannelTolerance;
        difference.differingPixels += differs ? 1u : 0u;

        for (std::size_t channel = 0; channel < kColorChannels; ++channel)
        {
            const std::uint8_t dimmed = static_cast<std::uint8_t>(expected.rgba[first + channel] / kHighlightDimming);
            const std::uint8_t red = channel == kRedChannel ? kFullChannel : std::uint8_t{0u};
            difference.highlight.rgba[first + channel] = differs ? red : dimmed;
        }
        difference.highlight.rgba[first + kColorChannels] = kFullChannel;
    }
    return difference;
}

bool SamePicture(const ImageDifference &difference)
{
    const std::size_t pixels = PixelCount(difference.highlight);
    if (pixels == 0u)
    {
        return false;
    }
    return static_cast<double>(difference.differingPixels) <= kGoldenDifferingShare * static_cast<double>(pixels);
}

std::string GoldenDeviceFolder(std::string_view deviceName)
{
    std::string folder;
    bool gap = false;
    for (const char character : deviceName)
    {
        if (!IsNameCharacter(character))
        {
            gap = true;
            continue;
        }
        if (gap && !folder.empty())
        {
            folder += '-';
        }
        folder += character;
        gap = false;
    }
    return folder.empty() ? std::string(kUnnamedDevice) : folder;
}

std::filesystem::path GoldenImagePath(const std::filesystem::path &directory, std::string_view deviceName,
                                      std::string_view levelPath)
{
    std::string name = std::filesystem::path(levelPath).stem().string();
    if (name.empty())
    {
        name = kUnnamedLevel;
    }
    return directory / GoldenDeviceFolder(deviceName) / (name + std::string(kImageExtension));
}

std::filesystem::path DiffImagePath(const std::filesystem::path &imagePath)
{
    std::filesystem::path diff = imagePath;
    diff.replace_extension();
    diff += kDiffSuffix;
    return diff;
}

std::optional<Render::CapturedImage> ReadImage(const std::filesystem::path &file)
{
    std::expected<Image::DecodedImage, Core::AssetError> decoded = Image::DecodeImageFile(file);
    if (!decoded || decoded->mips.empty())
    {
        return std::nullopt;
    }
    return Render::CapturedImage{
        .rgba = std::move(decoded->mips.front()), .width = decoded->width, .height = decoded->height};
}

bool CheckGoldenImage(const Render::CapturedImage &image, const std::filesystem::path &golden,
                      const std::filesystem::path &diffPath, bool accept)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(golden, error);
    if (accept || !exists)
    {
        std::filesystem::create_directories(golden.parent_path(), error);
        if (!Render::FrameCapture::WritePng(image, golden.string()))
        {
            return false;
        }
        Core::Log::Info("GoldenImage: {} {}.", exists ? "accepted the picture as" : "no golden picture yet; accepted",
                        golden.string());
        return true;
    }

    const std::optional<Render::CapturedImage> expected = ReadImage(golden);
    if (!expected)
    {
        return false;
    }
    const ImageDifference difference = CompareImages(image, *expected);
    if (SamePicture(difference))
    {
        Core::Log::Info("GoldenImage: matches {} ({} pixels differ, by up to {} of 255).", golden.string(),
                        difference.differingPixels, difference.largestDifference);
        return true;
    }
    if (difference.highlight.rgba.empty())
    {
        Core::Log::Error("GoldenImage: the picture is {}x{} but {} is {}x{}. If the new size is intended, set "
                         "ASSISI_ACCEPT_GOLDEN=1 to accept it.",
                         image.width, image.height, golden.string(), expected->width, expected->height);
        return false;
    }
    (void)Render::FrameCapture::WritePng(difference.highlight, diffPath.string());
    Core::Log::Error("GoldenImage: the picture differs from {} in {} pixels, by up to {} of 255; they are red in {}. "
                     "If the change is intended, set ASSISI_ACCEPT_GOLDEN=1 to accept it.",
                     golden.string(), difference.differingPixels, difference.largestDifference, diffPath.string());
    return false;
}

} // namespace Assisi::Editor
