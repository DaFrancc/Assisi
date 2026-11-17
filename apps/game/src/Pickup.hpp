/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Pickup.hpp
/// @brief Something a player picks up by walking into it, holds at a joint of
///        their body, and drops with Q: the book's example of holding
///        something at a joint in a game every machine plays.
///
/// The host decides who holds what, in Holdable::holder, which every machine
/// is sent. Parent links are not sent, so each machine hangs the item from the
/// holder's body itself, with a Parent and a JointAttachment.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Math/GLM.hpp>

#include <string_view>

namespace Game
{

/// @brief An item a player picks up by walking into it. Give it a Transform, a
/// MeshRenderer and Replicated, and leave it free in the level.
ACOMP(replicable)
struct Holdable
{
    /// The joint of the holder's body it is held at.
    AFIELD() Assisi::Core::InternedString joint{std::string_view{"hand_r"}};
    /// The character holding it, or NullEntity while it lies free.
    AFIELD() Assisi::ECS::Entity holder{Assisi::ECS::NullEntity};
    /// The character that dropped it last, who can't pick it up again until
    /// they have walked away from it. Only the host reads it.
    AFIELD(norep) Assisi::ECS::Entity droppedBy{Assisi::ECS::NullEntity};
    /// Where it sits from the joint while it is held (m).
    AFIELD() glm::vec3 offset{0.f, 0.1f, 0.f};
};

/// @brief A player asking the host to drop what they hold.
AMSG(intent, reliable)
struct DropHeld
{
    /// The asking player's character, which the host checks is theirs.
    AFIELD(controlled) Assisi::ECS::Entity dropper{Assisi::ECS::NullEntity};
};

/// The host gives a free item to the first player who walks into it, and drops
/// an item whose holder has left.
ASYSTEM(Update, name = "PickUpHoldables")
void PickUpHoldablesSystem(Assisi::App::SystemContext &ctx);

/// Q asks the host to drop what this machine's player holds. `activeWorldOnly`,
/// and it null-checks input: a headless host has no keyboard.
ASYSTEM(Update, name = "AskToDrop", activeWorldOnly)
void AskToDropSystem(Assisi::App::SystemContext &ctx);

/// The host drops what each asking player holds, on the floor in front of them.
ASYSTEM(Update, name = "DropHoldables", after = "PickUpHoldables")
void DropHoldablesSystem(Assisi::App::SystemContext &ctx);

/// Every machine hangs each held item from its holder's body at its joint, and
/// lets go of one that is free.
ASYSTEM(Update, name = "HoldHoldables", after = "DropHoldables")
void HoldHoldablesSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
