/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/ReplicationConfig.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <nlohmann/json.hpp>

#include <expected>
#include <string>

#include "ReplicationInternal.hpp"

// ===========================================================================
// The relevancy config loader.
// ===========================================================================

namespace Assisi::NetSync
{
std::vector<std::string> LoadNeverReplicateFromConfig(std::string_view configPath)
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(configPath);
    if (!text)
        return {}; // no config is not a problem; replicating everything capable is a complete answer

    try
    {
        const nlohmann::json json = nlohmann::json::parse(*text);
        if (!json.contains("networking"))
            return {};

        const nlohmann::json &block = json.at("networking");
        if (!block.contains("neverReplicate"))
            return {};

        const nlohmann::json &list = block.at("neverReplicate");
        if (!list.is_array())
        {
            Core::Log::Warn("NetSync: 'networking.neverReplicate' in '{}' must be an array of component names — "
                            "ignoring it.",
                            configPath);
            return {};
        }

        std::vector<std::string> names;
        for (const nlohmann::json &element : list)
        {
            if (element.is_string())
                names.push_back(element.get<std::string>());
            else
                Core::Log::Warn("NetSync: 'networking.neverReplicate' entries must be component names — skipping "
                                "a '{}'.",
                                element.type_name());
        }
        return names;
    }
    catch (const std::exception &error)
    {
        // A malformed config must not be able to silently *widen* what a game
        // sends, so say so rather than falling through quietly.
        Core::Log::Warn("NetSync: cannot read 'networking.neverReplicate' from '{}' ({}) — replicating every "
                        "capable component.",
                        configPath, error.what());
        return {};
    }
}

RelevancyConfig LoadRelevancyFromConfig(std::string_view configPath)
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(configPath);
    if (!text)
        return {}; // no config is not a problem; telling everyone everything is a complete answer

    try
    {
        const nlohmann::json json = nlohmann::json::parse(*text);
        if (!json.contains("networking"))
            return {};

        const nlohmann::json &block = json.at("networking");
        if (!block.contains("relevancy"))
            return {};

        const nlohmann::json &relevancy = block.at("relevancy");
        if (!relevancy.is_object())
        {
            Core::Log::Warn("NetSync: 'networking.relevancy' in '{}' must be an object — ignoring it.", configPath);
            return {};
        }

        RelevancyConfig config;
        if (relevancy.contains("provider"))
        {
            const std::string name = relevancy.at("provider").get<std::string>();
            if (name == "all")
            {
                config.provider = RelevancyConfig::Provider::All;
            }
            else if (name == "distance")
            {
                config.provider = RelevancyConfig::Provider::Distance;
            }
            else
            {
                // A name nobody implements is a typo or a renamed provider, and
                // quietly falling back to "everything" would leave the author
                // believing a radius is in force when it is not.
                Core::Log::Warn("NetSync: 'networking.relevancy.provider' is '{}', which is not a provider this "
                                "build knows ('all' or 'distance') — telling every connection about everything.",
                                name);
            }
        }
        if (relevancy.contains("radius"))
            config.radius = relevancy.at("radius").get<float>();
        if (relevancy.contains("exitRadius"))
            config.exitRadius = relevancy.at("exitRadius").get<float>();
        if (relevancy.contains("dwellTicks"))
            config.dwellTicks = relevancy.at("dwellTicks").get<std::uint32_t>();
        return config;
    }
    catch (const std::exception &error)
    {
        // Same direction as every other loader here: a malformed config must not
        // silently *narrow* what a game sends.
        Core::Log::Warn("NetSync: cannot read 'networking.relevancy' from '{}' ({}) — telling every connection "
                        "about everything.",
                        configPath, error.what());
        return {};
    }
}

} // namespace Assisi::NetSync
