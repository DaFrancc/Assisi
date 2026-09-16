/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/AssetSource.hpp>

#include <utility>

namespace Assisi::Render
{

namespace
{

const AssetSource *&Installed()
{
    static const AssetSource *source = nullptr;
    return source;
}

} // namespace

std::string_view ToString(AssetLoadError error) noexcept
{
    switch (error)
    {
    case AssetLoadError::UnknownAsset:
        return "no such asset";
    case AssetLoadError::Unreadable:
        return "its bytes could not be read";
    case AssetLoadError::Undecodable:
        return "its bytes are not that kind of asset";
    }
    return "unknown";
}

const AssetSource *SetAssetSource(const AssetSource *source)
{
    return std::exchange(Installed(), source);
}

const AssetSource *GetAssetSource()
{
    return Installed();
}

} // namespace Assisi::Render
