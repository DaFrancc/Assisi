/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/AppConfig.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <expected>
#include <string>

namespace Assisi::App
{

AppConfig AppConfig::FromJsonText(std::string_view text)
{
    AppConfig cfg;
    const std::expected<void, Core::Reflect::AssetDocumentError> applied =
        Core::Reflect::ApplyAssetDocument(text, cfg);
    if (!applied)
    {
        Core::Log::Warn("App: cannot read the game config ({}) — using defaults.",
                        Core::Reflect::ToString(applied.error()));
        return AppConfig{};
    }

    // A zero rate silently disables fixed update (step = inf) and a negative one
    // makes the accumulator loop in Application::Run non-terminating, so reject
    // both here rather than trusting the config file.
    if (cfg.physicsHz <= 0.0)
    {
        const double fallbackHz = AppConfig{}.physicsHz;
        Core::Log::Warn("App: physicsHz = {} is invalid (must be > 0) — using {} Hz.", cfg.physicsHz, fallbackHz);
        cfg.physicsHz = fallbackHz;
    }

    return cfg;
}

AppConfig AppConfig::Load(std::string_view assetPath)
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        Core::Log::Warn("App: '{}' not found — using default engine configuration.", assetPath);
        return AppConfig{};
    }
    return FromJsonText(*text);
}

} // namespace Assisi::App
