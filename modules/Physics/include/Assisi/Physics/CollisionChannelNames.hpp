/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file CollisionChannelNames.hpp
/// @brief How a game takes collision channels of its own and names them.
///
/// A game declares each channel it uses once, in a header under its src/, and
/// lists the names beside the aliases:
///
/// @code
/// inline constexpr Assisi::Physics::CollisionChannel Bullet = Assisi::Physics::GameChannel(0);
/// inline constexpr Assisi::Physics::CollisionChannel Pickup = Assisi::Physics::GameChannel(1);
///
/// inline constexpr Assisi::Physics::ChannelName kChannelNames[] = {
///     {"Bullet", Bullet},
///     {"Pickup", Pickup},
/// };
/// ASSISI_COLLISION_CHANNEL_NAMES(kChannelNames);
/// @endcode
///
/// Code uses the aliases; the editor shows the names wherever it shows a channel
/// or a mask, and hides the game slots nobody named.

#include <Assisi/Physics/PhysicsComponents.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Assisi::Physics
{

/// The name reflection knows CollisionChannel by, which is what the editor asks
/// for its labels under.
inline constexpr std::string_view kCollisionChannelEnumType = "Assisi::Physics::CollisionChannel";

/// One game channel and the name the editor shows for it.
struct ChannelName
{
    const char *name = "";
    CollisionChannel channel = CollisionChannel::World;
};

/// @brief Whether @p names can all stand: each names a game slot rather than one
/// of the engine's own channels, has a name, and shares neither its slot nor its
/// name with another.
consteval bool ValidChannelNames(std::span<const ChannelName> names)
{
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (static_cast<std::uint32_t>(names[i].channel) < kFirstGameChannel || std::string_view(names[i].name).empty())
        {
            return false;
        }
        for (std::size_t j = i + 1; j < names.size(); ++j)
        {
            if (names[i].channel == names[j].channel || std::string_view(names[i].name) == names[j].name)
            {
                return false;
            }
        }
    }
    return true;
}

/// @brief Hands @p names to the editor when constructed. Made by
/// ASSISI_COLLISION_CHANNEL_NAMES rather than by hand, so the table is always
/// checked first.
class ChannelNameRegistration
{
public:
    explicit ChannelNameRegistration(std::span<const ChannelName> names);
};

} // namespace Assisi::Physics

/// @brief Checks the game's channel table @p table at compile time and
/// registers its names.
///
/// Belongs in a header under the game's src/. Every header there is compiled
/// into both the game and the editor through their reflection, which is what
/// keeps this registration from being dropped as unreferenced.
#define ASSISI_COLLISION_CHANNEL_NAMES(table)                                                                     \
        static_assert(::Assisi::Physics::ValidChannelNames(table),                                                   \
                      "Each game channel needs a name, a slot from GameChannel, and neither shared with another."); \
        inline const ::Assisi::Physics::ChannelNameRegistration table ## Registration { table }
