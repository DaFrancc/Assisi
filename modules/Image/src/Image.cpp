/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Image/Image.hpp>

namespace Assisi::Image
{

bool ValidateMipChain(const DecodedImage &image)
{
    if (image.mips.empty() || image.width == 0 || image.height == 0)
    {
        return false;
    }
    if (image.format == PixelFormat::Count)
    {
        return false;
    }

    const std::uint32_t levels = static_cast<std::uint32_t>(image.mips.size());
    if (levels > MipLevelCount(image.width, image.height))
    {
        return false;
    }

    for (std::uint32_t level = 0; level < levels; ++level)
    {
        if (image.mips[level].empty())
        {
            continue;
        }
        const LevelLayout layout =
            LayoutFor(image.format, MipExtent(image.width, level), MipExtent(image.height, level));
        if (image.mips[level].size() != layout.byteSize)
        {
            return false;
        }
    }
    return true;
}

} // namespace Assisi::Image
