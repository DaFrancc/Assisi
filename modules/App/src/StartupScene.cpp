/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/StartupScene.hpp>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetSystem.hpp>

#include <optional>

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
                                                                  const Core::AssetDatabase &database)
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
        const std::optional<std::string> path = database.PathFor(*id);
        if (!path)
        {
            return std::unexpected(StartupSceneError::UnknownGuid);
        }
        return *path;
    }

    if (!Core::AssetSystem::Exists(named))
    {
        return std::unexpected(StartupSceneError::Missing);
    }
    return std::string(named);
}

} // namespace Assisi::App
