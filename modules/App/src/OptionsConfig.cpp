/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/OptionsConfig.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace Assisi::App
{

static Render::AaMode AaModeFromString(const std::string &str)
{
    if (str == "msaa")
        return Render::AaMode::MSAA;
    if (str == "fxaa")
        return Render::AaMode::FXAA;
    if (str == "msaa+fxaa")
        return Render::AaMode::MSAA_FXAA;
    return Render::AaMode::None;
}

static const char *AaModeToString(Render::AaMode mode)
{
    switch (mode)
    {
    case Render::AaMode::MSAA:
        return "msaa";
    case Render::AaMode::FXAA:
        return "fxaa";
    case Render::AaMode::MSAA_FXAA:
        return "msaa+fxaa";
    default:
        return "none";
    }
}

static Render::TonemapOperator TonemapOperatorFromString(const std::string &str)
{
    if (str == "aces")
        return Render::TonemapOperator::Aces;
    if (str == "reinhard")
        return Render::TonemapOperator::Reinhard;
    return Render::TonemapOperator::AgX;
}

static const char *TonemapOperatorToString(Render::TonemapOperator op)
{
    switch (op)
    {
    case Render::TonemapOperator::Aces:
        return "aces";
    case Render::TonemapOperator::Reinhard:
        return "reinhard";
    default:
        return "agx";
    }
}

static Render::ShadowFilter ShadowFilterFromString(const std::string &str)
{
    if (str == "point")
        return Render::ShadowFilter::Point;
    if (str == "pcf5x5")
        return Render::ShadowFilter::Pcf5x5;
    if (str == "vogel")
        return Render::ShadowFilter::Vogel;
    return Render::ShadowFilter::Pcf3x3;
}

static const char *ShadowFilterToString(Render::ShadowFilter filter)
{
    switch (filter)
    {
    case Render::ShadowFilter::Point:
        return "point";
    case Render::ShadowFilter::Pcf5x5:
        return "pcf5x5";
    case Render::ShadowFilter::Vogel:
        return "vogel";
    default:
        return "pcf3x3";
    }
}

static Render::ShadowMapFormat ShadowFormatFromString(const std::string &str)
{
    return str == "d16" ? Render::ShadowMapFormat::D16 : Render::ShadowMapFormat::D32;
}

static const char *ShadowFormatToString(Render::ShadowMapFormat format)
{
    return format == Render::ShadowMapFormat::D16 ? "d16" : "d32";
}

/// @brief Reads @p key into @p field when the object has it, and leaves the
/// default when it does not.
///
/// A missing key is the normal case — options.json only ever holds what someone
/// changed — so absence is not an error. A key of the wrong type throws, which
/// the caller's handler turns into one warning and a whole-file fallback; that
/// is deliberate, since a file that has been mangled badly enough to have a
/// string where a float goes is not one to trust field by field.
template <typename T> void ReadField(const nlohmann::json &json, const char *key, T &field)
{
    if (const auto entry = json.find(key); entry != json.end())
    {
        field = entry->get<T>();
    }
}

/// @brief The same, for a field stored as a string and mapped by @p parse.
///
/// Each parse function falls back to its own default for an unrecognised
/// string, so a typo costs that one field rather than the file.
template <typename T, typename Parse>
void ReadMapped(const nlohmann::json &json, const char *key, T &field, Parse parse)
{
    if (const auto entry = json.find(key); entry != json.end())
    {
        field = parse(entry->get<std::string>());
    }
}

OptionsConfig OptionsConfig::FromJsonText(std::string_view text)
{
    OptionsConfig cfg;

    try
    {
        const nlohmann::json json = nlohmann::json::parse(text);

        if (json.contains("antiAliasing"))
        {
            const auto &aa = json.at("antiAliasing");
            if (aa.contains("mode"))
            {
                cfg.aaMode = AaModeFromString(aa.at("mode").get<std::string>());
            }
            if (aa.contains("msaaSamples"))
            {
                const int32_t samples = aa.at("msaaSamples").get<int32_t>();
                if (samples == 2 || samples == 4 || samples == 8)
                {
                    cfg.msaaSamples = samples;
                }
            }
        }

        if (json.contains("toneMap"))
        {
            const auto &tm = json.at("toneMap");
            ReadMapped(tm, "operator", cfg.tonemap.op, TonemapOperatorFromString);
            ReadField(tm, "exposureStops", cfg.tonemap.exposureStops);
            ReadField(tm, "contrast", cfg.tonemap.contrast);
            ReadField(tm, "saturation", cfg.tonemap.saturation);
            // Whatever the file said, the shader only ever sees values in range.
            cfg.tonemap = Render::Sanitized(cfg.tonemap);
        }

        if (json.contains("shadows"))
        {
            const auto &sh = json.at("shadows");
            Render::ShadowSettings &shadows = cfg.shadows;
            if (sh.contains("sun"))
            {
                const auto &sun = sh.at("sun");
                ReadField(sun, "enabled", shadows.sun.enabled);
                ReadField(sun, "cascades", shadows.sun.cascadeCount);
                ReadField(sun, "resolution", shadows.sun.resolution);
                ReadMapped(sun, "format", shadows.sun.format, ShadowFormatFromString);
                ReadField(sun, "maxDistance", shadows.sun.maxDistance);
                ReadField(sun, "splitLambda", shadows.sun.splitLambda);
                ReadMapped(sun, "filter", shadows.sun.filter, ShadowFilterFromString);
                ReadField(sun, "depthBiasTexels", shadows.sun.depthBiasTexels);
                ReadField(sun, "slopeBias", shadows.sun.slopeBias);
                ReadField(sun, "normalOffsetTexels", shadows.sun.normalOffsetTexels);
                ReadField(sun, "cascadeBlend", shadows.sun.cascadeBlend);
            }
            if (sh.contains("local"))
            {
                const auto &local = sh.at("local");
                ReadField(local, "enabled", shadows.local.enabled);
                ReadField(local, "atlasResolution", shadows.local.atlasResolution);
                ReadMapped(local, "format", shadows.local.format, ShadowFormatFromString);
                ReadField(local, "faceResolution", shadows.local.faceResolution);
                ReadMapped(local, "filter", shadows.local.filter, ShadowFilterFromString);
                ReadField(local, "depthBiasTexels", shadows.local.depthBiasTexels);
                ReadField(local, "slopeBias", shadows.local.slopeBias);
                ReadField(local, "normalOffsetTexels", shadows.local.normalOffsetTexels);
                if (local.contains("cache"))
                {
                    const auto &cache = local.at("cache");
                    ReadField(cache, "enabled", shadows.local.cache.enabled);
                    ReadField(cache, "updateBudgetFaces", shadows.local.cache.updateBudgetFaces);
                    ReadField(cache, "promoteStillFrames", shadows.local.cache.promoteStillFrames);
                    ReadField(cache, "movingLightUpdateDivisor", shadows.local.cache.movingLightUpdateDivisor);
                }
            }
            if (sh.contains("selection"))
            {
                const auto &selection = sh.at("selection");
                ReadField(selection, "capEnabled", shadows.selection.capEnabled);
                ReadField(selection, "capSpot", shadows.selection.capSpot);
                ReadField(selection, "capPoint", shadows.selection.capPoint);
                ReadField(selection, "capHysteresis", shadows.selection.capHysteresis);
                ReadField(selection, "classHysteresis", shadows.selection.classHysteresis);
            }
            // Whatever the file said, nothing downstream sees an out-of-range
            // value — and here that is a texture allocation, not only a shader.
            shadows = Render::Sanitized(shadows);
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
                const int32_t limit = fs.at("fpsLimit").get<int32_t>();
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

OptionsConfig OptionsConfig::LoadFromJson()
{
    // options.json is per-user writable state, so it lives under the user root
    // (see SaveToJson), not the read-only asset root.
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadUserText("options.json");
    if (!text)
    {
        return OptionsConfig{};
    }
    return FromJsonText(*text);
}

std::string OptionsConfig::ToJsonText() const
{
    nlohmann::json json;
    json["antiAliasing"]["mode"] = AaModeToString(aaMode);
    json["antiAliasing"]["msaaSamples"] = msaaSamples;
    json["toneMap"]["operator"] = TonemapOperatorToString(tonemap.op);
    json["toneMap"]["exposureStops"] = tonemap.exposureStops;
    json["toneMap"]["contrast"] = tonemap.contrast;
    json["toneMap"]["saturation"] = tonemap.saturation;

    nlohmann::json &sun = json["shadows"]["sun"];
    sun["enabled"] = shadows.sun.enabled;
    sun["cascades"] = shadows.sun.cascadeCount;
    sun["resolution"] = shadows.sun.resolution;
    sun["format"] = ShadowFormatToString(shadows.sun.format);
    sun["maxDistance"] = shadows.sun.maxDistance;
    sun["splitLambda"] = shadows.sun.splitLambda;
    sun["filter"] = ShadowFilterToString(shadows.sun.filter);
    sun["depthBiasTexels"] = shadows.sun.depthBiasTexels;
    sun["slopeBias"] = shadows.sun.slopeBias;
    sun["normalOffsetTexels"] = shadows.sun.normalOffsetTexels;
    sun["cascadeBlend"] = shadows.sun.cascadeBlend;

    nlohmann::json &local = json["shadows"]["local"];
    local["enabled"] = shadows.local.enabled;
    local["atlasResolution"] = shadows.local.atlasResolution;
    local["format"] = ShadowFormatToString(shadows.local.format);
    local["faceResolution"] = shadows.local.faceResolution;
    local["filter"] = ShadowFilterToString(shadows.local.filter);
    local["depthBiasTexels"] = shadows.local.depthBiasTexels;
    local["slopeBias"] = shadows.local.slopeBias;
    local["normalOffsetTexels"] = shadows.local.normalOffsetTexels;

    nlohmann::json &cache = json["shadows"]["local"]["cache"];
    cache["enabled"] = shadows.local.cache.enabled;
    cache["updateBudgetFaces"] = shadows.local.cache.updateBudgetFaces;
    cache["promoteStillFrames"] = shadows.local.cache.promoteStillFrames;
    cache["movingLightUpdateDivisor"] = shadows.local.cache.movingLightUpdateDivisor;

    nlohmann::json &selection = json["shadows"]["selection"];
    selection["capEnabled"] = shadows.selection.capEnabled;
    selection["capSpot"] = shadows.selection.capSpot;
    selection["capPoint"] = shadows.selection.capPoint;
    selection["capHysteresis"] = shadows.selection.capHysteresis;
    selection["classHysteresis"] = shadows.selection.classHysteresis;

    json["frameSync"]["mode"] = (frameSync == FrameSyncMode::FpsLimit) ? "fpsLimit" : "vsync";
    json["frameSync"]["fpsLimit"] = fpsLimit;

    return json.dump(4);
}

void OptionsConfig::SaveToJson() const
{
    const std::expected<void, Core::AssetError> result = Core::AssetSystem::WriteText("options.json", ToJsonText());
    if (!result)
    {
        Core::Log::Warn("Could not write options.json (asset error {}).", static_cast<int32_t>(result.error()));
    }
}

} // namespace Assisi::App