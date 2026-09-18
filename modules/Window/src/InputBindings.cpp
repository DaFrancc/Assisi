/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Window/InputBindings.hpp>

#include <Assisi/Core/ConfigReader.hpp>
#include <Assisi/Core/Logger.hpp>

#include <expected>
#include <string>

namespace Assisi::Window
{

std::expected<InputBindings, Core::ConfigError> LoadInputBindings(std::string_view assetPath)
{
    InputBindings bindings;
    const std::expected<void, Core::ConfigError> read = Core::ReadConfig(assetPath, bindings);
    if (!read)
    {
        if (read.error() == Core::ConfigError::Missing)
        {
            return InputBindings{};
        }
        Core::Log::Warn("Window: cannot read input bindings from '{}' ({}) - no actions are bound.", assetPath,
                        Core::ToString(read.error()));
        return std::unexpected(read.error());
    }

    return bindings;
}

} // namespace Assisi::Window
