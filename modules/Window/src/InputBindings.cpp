/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Window/InputBindings.hpp>

#include <Assisi/Core/AssetSystem.hpp>
#include <Assisi/Core/Logger.hpp>

#include <expected>
#include <string>

namespace Assisi::Window
{

std::expected<InputBindings, Core::Reflect::AssetDocumentError> LoadInputBindings(std::string_view assetPath)
{
    const std::expected<std::string, Core::AssetError> text = Core::AssetSystem::ReadText(assetPath);
    if (!text)
    {
        // Not an error worth a log line here: a build with no bindings file has
        // no bindings, which the caller can see from the empty result and
        // report in the terms its own layer uses.
        return InputBindings{};
    }

    InputBindings bindings;
    const std::expected<void, Core::Reflect::AssetDocumentError> applied =
        Core::Reflect::ApplyAssetDocument(*text, bindings);
    if (!applied)
    {
        Core::Log::Warn("Window: cannot read input bindings from '{}' ({}) — no actions are bound.", assetPath,
                        Core::Reflect::ToString(applied.error()));
        return std::unexpected(applied.error());
    }

    return bindings;
}

} // namespace Assisi::Window
