/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file CollisionChannelNames.cpp
/// @brief Hands a game's channel names to the editor's label registry.

#include <Assisi/Physics/CollisionChannelNames.hpp>

#include <Assisi/Core/Reflect/EnumLabels.hpp>

namespace Assisi::Physics
{

ChannelNameRegistration::ChannelNameRegistration(std::span<const ChannelName> names)
{
    for (const ChannelName &entry : names)
    {
        Core::Reflect::RegisterEnumLabel(kCollisionChannelEnumType, static_cast<std::int64_t>(entry.channel),
                                         entry.name);
    }
}

} // namespace Assisi::Physics
