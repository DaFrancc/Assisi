/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/AppConfig.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <nlohmann/json.hpp>

#include <fstream>

namespace Assisi::App
{

AppConfig AppConfig::LoadFromJson()
{
    AppConfig cfg;

    const auto pathResult = Core::AssetSystem::Resolve("game.json");
    if (!pathResult)
    {
        Core::Log::Warn("game.json not found — using default engine configuration.");
        return cfg;
    }

    std::ifstream file(pathResult.value());
    if (!file.is_open())
    {
        Core::Log::Warn("Could not open game.json — using default engine configuration.");
        return cfg;
    }

    try
    {
        const auto json = nlohmann::json::parse(file);

        if (json.contains("window"))
        {
            const auto &w = json.at("window");
            if (w.contains("title"))  cfg.title  = w.at("title").get<std::string>();
            if (w.contains("width"))  cfg.width  = w.at("width").get<int>();
            if (w.contains("height")) cfg.height = w.at("height").get<int>();
        }

        if (json.contains("render"))
        {
            const auto &r = json.at("render");
            if (r.contains("clearColor"))
            {
                const auto &c = r.at("clearColor");
                if (c.is_array() && c.size() == 4)
                {
                    cfg.clearColor = {c[0].get<float>(), c[1].get<float>(),
                                      c[2].get<float>(), c[3].get<float>()};
                }
            }
        }

        if (json.contains("timing"))
        {
            const auto &t = json.at("timing");
            if (t.contains("physicsHz")) cfg.physicsHz = t.at("physicsHz").get<double>();
            if (t.contains("renderHz"))  cfg.renderHz  = t.at("renderHz").get<double>();
        }
    }
    catch (const nlohmann::json::exception &e)
    {
        Core::Log::Warn("Failed to parse game.json: {} — using defaults.", e.what());
    }

    return cfg;
}

} // namespace Assisi::App