/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/ConfigReader.hpp>

#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/CookedPayload.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/AssetDocument.hpp>
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>

#include <string>
#include <utility>
#include <vector>

namespace Assisi::Core
{

namespace
{

ConfigReader &InstalledReader()
{
    static ConfigReader reader;
    return reader;
}

} // namespace

std::string_view ToString(ConfigError error) noexcept
{
    switch (error)
    {
    case ConfigError::Missing:
        return "no config is stored at that path";
    case ConfigError::Unreadable:
        return "the config could not be read";
    case ConfigError::Malformed:
        return "the config is not a document of the expected type";
    }
    return "unknown";
}

ConfigReader SetConfigReader(ConfigReader reader)
{
    return std::exchange(InstalledReader(), std::move(reader));
}

std::expected<void, ConfigError> ReadConfig(std::string_view vpath, std::type_index type, void *instance)
{
    const ConfigReader &reader = InstalledReader();
    if (!reader)
    {
        Log::Error("Config: no config reader is installed, so '{}' cannot be read.", vpath);
        return std::unexpected(ConfigError::Unreadable);
    }
    return reader(vpath, type, instance);
}

std::expected<void, ConfigError> ReadCookedConfig(const AssetProvider &provider, std::string_view vpath,
                                                  std::type_index type, void *instance)
{
    const std::expected<AssetId, AssetError> id = provider.Resolve(vpath);
    if (!id)
    {
        return std::unexpected(id.error() == AssetError::UnknownAssetId ? ConfigError::Missing
                                                                        : ConfigError::Unreadable);
    }
    const std::expected<std::vector<std::byte>, AssetError> bytes = provider.Open(*id);
    if (!bytes)
    {
        return std::unexpected(ConfigError::Unreadable);
    }
    const Reflect::AssetTypeMeta *meta = Reflect::AssetTypeRegistry::Instance().Find(type);
    if (meta == nullptr || !ReadReflectedBlob(*bytes, *meta, instance))
    {
        return std::unexpected(ConfigError::Malformed);
    }
    return {};
}

} // namespace Assisi::Core
