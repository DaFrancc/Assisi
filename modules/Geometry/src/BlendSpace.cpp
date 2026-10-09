/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Geometry/BlendSpace.hpp>

#include <Assisi/Core/Reflect/AssetDocument.hpp>

#include <cmath>
#include <cstddef>
#include <string_view>

namespace Assisi::Geometry
{

namespace
{

bool Finite(const glm::vec2 &position)
{
    return std::isfinite(position.x) && std::isfinite(position.y);
}

bool SharesPosition(std::span<const BlendPoint> points)
{
    for (std::size_t first = 0; first < points.size(); ++first)
    {
        for (std::size_t second = first + 1; second < points.size(); ++second)
        {
            if (glm::distance(points[first].Position, points[second].Position) < kBlendPointSpacing)
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

std::string_view ToString(BlendSpaceError error) noexcept
{
    switch (error)
    {
    case BlendSpaceError::NotADocument:
        return "not a blend space document";
    case BlendSpaceError::NoPoints:
        return "the space has no points";
    case BlendSpaceError::PointHasNoClip:
        return "a point names no clip";
    case BlendSpaceError::BadPosition:
        return "a point's position is not a finite number";
    case BlendSpaceError::SharedPosition:
        return "two points share a position";
    case BlendSpaceError::Count_:
        break;
    }
    return "unknown error";
}

Core::AssetError ToAssetError(BlendSpaceError error) noexcept
{
    return Core::AssetError{Core::AssetErrorCode::CorruptAsset, ToString(error)};
}

std::expected<BlendSpace, BlendSpaceError> ReadBlendSpace(std::span<const std::byte> bytes)
{
    const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    BlendSpace space;
    if (!Core::Reflect::ApplyAssetDocument(text, space))
    {
        return std::unexpected(BlendSpaceError::NotADocument);
    }
    if (space.Points.empty())
    {
        return std::unexpected(BlendSpaceError::NoPoints);
    }
    for (const BlendPoint &point : space.Points)
    {
        if (point.Clip.IsNil())
        {
            return std::unexpected(BlendSpaceError::PointHasNoClip);
        }
        if (!Finite(point.Position))
        {
            return std::unexpected(BlendSpaceError::BadPosition);
        }
    }
    if (SharesPosition(space.Points))
    {
        return std::unexpected(BlendSpaceError::SharedPosition);
    }
    return space;
}

} // namespace Assisi::Geometry
