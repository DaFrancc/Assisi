/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/StartupScene.hpp>

#include <Assisi/Core/AssetId.hpp>

#include <optional>
#include <utility>

namespace Assisi::App
{

std::string_view Describe(StartupSceneError error)
{
    switch (error)
    {
        case StartupSceneError::Unnamed:
            return "the config names no startup scene";
        case StartupSceneError::UnknownGuid:
            return "no asset in this install carries that id";
        case StartupSceneError::Missing:
            return "no readable file at that path";
    }
    return "unknown";
}

std::expected<std::string, StartupSceneError> ResolveStartupScene(std::string_view named,
                                                                  const ScenePathForId &pathFor,
                                                                  const SceneExists &exists)
{
    if (named.empty())
    {
        return std::unexpected(StartupSceneError::Unnamed);
    }

    // An id before a path: the two forms cannot be confused (a UUID holds no
    // '/' and no extension), and trying the path first would report a deleted
    // asset as a missing file named after a GUID.
    if (const std::optional<Core::AssetId> id = Core::AssetId::Parse(named))
    {
        std::optional<std::string> path = pathFor(*id);
        if (!path)
        {
            return std::unexpected(StartupSceneError::UnknownGuid);
        }
        return std::move(*path);
    }

    if (!exists(named))
    {
        return std::unexpected(StartupSceneError::Missing);
    }
    return std::string(named);
}

} // namespace Assisi::App
