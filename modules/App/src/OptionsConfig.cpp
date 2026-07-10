/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/OptionsConfig.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace Assisi::App
{

static Render::AaMode AaModeFromString(const std::string &str)
{
    if (str == "msaa")       return Render::AaMode::MSAA;
    if (str == "fxaa")       return Render::AaMode::FXAA;
    if (str == "msaa+fxaa")  return Render::AaMode::MSAA_FXAA;
    return Render::AaMode::None;
}

static const char *AaModeToString(Render::AaMode mode)
{
    switch (mode)
    {
    case Render::AaMode::MSAA:      return "msaa";
    case Render::AaMode::FXAA:      return "fxaa";
    case Render::AaMode::MSAA_FXAA: return "msaa+fxaa";
    default:                        return "none";
    }
}

OptionsConfig OptionsConfig::LoadFromJson()
{
    OptionsConfig cfg;

    // options.json is per-user writable state, so it lives under the user root
    // (see SaveToJson), not the read-only asset root.
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadUserText("options.json");
    if (!text)
    {
        return cfg;
    }

    try
    {
        const nlohmann::json json = nlohmann::json::parse(*text);

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

        if (json.contains("frameSync"))
        {
            const auto &fs = json.at("frameSync");
            if (fs.contains("mode"))
            {
                cfg.frameSync =
                    (fs.at("mode").get<std::string>() == "fpsLimit") ? FrameSyncMode::FpsLimit : FrameSyncMode::VSync;
            }
            if (fs.contains("fpsLimit"))
            {
                // Accept only the -1 (unlimited) sentinel or a positive cap that
                // fits an int16 — reject 0 and out-of-range junk, keeping the default.
                const int limit = fs.at("fpsLimit").get<int>();
                if (limit == -1 || (limit > 0 && limit <= INT16_MAX))
                {
                    cfg.fpsLimit = static_cast<std::int16_t>(limit);
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
    json["frameSync"]["mode"]           = (frameSync == FrameSyncMode::FpsLimit) ? "fpsLimit" : "vsync";
    json["frameSync"]["fpsLimit"]       = fpsLimit;

    const std::expected<void, Core::AssetError> result = Core::AssetSystem::WriteText("options.json", json.dump(4));
    if (!result)
    {
        Core::Log::Warn("Could not write options.json (asset error {}).", static_cast<int>(result.error()));
    }
}

} // namespace Assisi::App