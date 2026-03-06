/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/OptionsConfig.hpp>

#include <Assisi/Core/Logger.hpp>

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace Assisi::App
{

static AaMode AaModeFromString(const std::string &str)
{
    if (str == "msaa")       return AaMode::MSAA;
    if (str == "fxaa")       return AaMode::FXAA;
    if (str == "msaa+fxaa")  return AaMode::MSAA_FXAA;
    return AaMode::None;
}

static const char *AaModeToString(AaMode mode)
{
    switch (mode)
    {
    case AaMode::MSAA:      return "msaa";
    case AaMode::FXAA:      return "fxaa";
    case AaMode::MSAA_FXAA: return "msaa+fxaa";
    default:                return "none";
    }
}

OptionsConfig OptionsConfig::LoadFromJson()
{
    OptionsConfig cfg;

    std::ifstream file("options.json");
    if (!file.is_open())
    {
        return cfg;
    }

    try
    {
        const auto json = nlohmann::json::parse(file);

        if (json.contains("antiAliasing"))
        {
            const auto &aa = json.at("antiAliasing");
            if (aa.contains("mode"))
            {
                cfg.aaMode = AaModeFromString(aa.at("mode").get<std::string>());
            }
            if (aa.contains("msaaSamples"))
            {
                const int samples = aa.at("msaaSamples").get<int>();
                if (samples == 2 || samples == 4 || samples == 8)
                {
                    cfg.msaaSamples = samples;
                }
            }
        }
    }
    catch (const nlohmann::json::exception &e)
    {
        Core::Log::Warn("Failed to parse options.json: {} — using defaults.", e.what());
    }

    return cfg;
}

void OptionsConfig::SaveToJson() const
{
    nlohmann::json json;
    json["antiAliasing"]["mode"]        = AaModeToString(aaMode);
    json["antiAliasing"]["msaaSamples"] = msaaSamples;

    std::ofstream file("options.json");
    if (!file.is_open())
    {
        Core::Log::Warn("Could not write options.json.");
        return;
    }

    file << json.dump(4);
}

} // namespace Assisi::App