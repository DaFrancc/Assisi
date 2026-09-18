/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

// The text document reader, alone in its file so a game that never installs it
// does not link it.

#include <Assisi/Runtime/SceneSerializer.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

namespace Assisi::Runtime
{

std::expected<nlohmann::json, LevelError> SceneSerializer::ReadTextDocument(std::string_view vpath)
{
    const auto text = Core::AssetSystem::ReadText(vpath);
    if (!text)
    {
        Core::Log::Error("SceneSerializer: cannot read asset '{}'", vpath);
        return std::unexpected(LevelError::FileUnreadable);
    }

    nlohmann::json doc = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/ false);
    if (doc.is_discarded())
    {
        Core::Log::Error("SceneSerializer: '{}' is not readable JSON", vpath);
        return std::unexpected(LevelError::MalformedJson);
    }
    return doc;
}

} // namespace Assisi::Runtime
