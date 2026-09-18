/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/AppConfig.hpp>

#include <Assisi/Core/ConfigReader.hpp>
#include <Assisi/Core/Logger.hpp>

#include <expected>
#include <string>

namespace Assisi::App
{

namespace
{

AppConfig Sanitized(AppConfig cfg)
{
    // A zero rate silently disables fixed update (step = inf) and a negative one
    // makes the accumulator loop in Application::Run non-terminating, so reject
    // both here rather than trusting the config file.
    if (cfg.physicsHz <= 0.0)
    {
        const double fallbackHz = AppConfig{}.physicsHz;
        Core::Log::Warn("App: physicsHz = {} is invalid (must be > 0) - using {} Hz.", cfg.physicsHz, fallbackHz);
        cfg.physicsHz = fallbackHz;
    }
    return cfg;
}

} // namespace

AppConfig AppConfig::FromJsonText(std::string_view text)
{
    AppConfig cfg;
    const std::expected<void, Core::Reflect::AssetDocumentError> applied =
        Core::Reflect::ApplyAssetDocument(text, cfg);
    if (!applied)
    {
        Core::Log::Warn("App: cannot read the game config ({}) - using defaults.",
                        Core::Reflect::ToString(applied.error()));
        return AppConfig{};
    }
    return Sanitized(cfg);
}

AppConfig AppConfig::Load(std::string_view assetPath)
{
    AppConfig cfg;
    const std::expected<void, Core::ConfigError> read = Core::ReadConfig(assetPath, cfg);
    if (!read)
    {
        Core::Log::Warn("App: cannot read '{}' ({}) - using default engine configuration.", assetPath,
                        Core::ToString(read.error()));
        return AppConfig{};
    }
    return Sanitized(cfg);
}

} // namespace Assisi::App
