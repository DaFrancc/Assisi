/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

// The text config reader, alone in its file so a game that never installs it
// does not link it.

#include <Assisi/Core/ConfigReader.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>

#include <string>

namespace Assisi::Core
{

std::expected<void, ConfigError> ReadTextConfig(std::string_view vpath, std::type_index type, void *instance)
{
    if (!AssetSystem::Exists(vpath))
    {
        return std::unexpected(ConfigError::Missing);
    }
    const std::expected<std::string, AssetError> text = AssetSystem::ReadText(vpath);
    if (!text)
    {
        return std::unexpected(ConfigError::Unreadable);
    }
    if (!Reflect::ApplyAssetDocument(*text, type, instance))
    {
        return std::unexpected(ConfigError::Malformed);
    }
    return {};
}

} // namespace Assisi::Core
