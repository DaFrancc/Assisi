/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Pickup.hpp"

#include <Assisi/App/Network.hpp>
#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Received.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <utility>
#include <vector>

namespace Game
{

namespace
{

using Assisi::ECS::Entity;
using Assisi::ECS::NullEntity;

/// How close a player's feet come to a free item, across the ground, to pick
/// it up (m).
constexpr double kReach = 0.7;

/// How far the player who dropped an item walks from it, across the ground,
/// before they can pick it up again (m). Further than kReach, so dropping it
/// at their feet doesn't hand it straight back.
constexpr double kWalkAway = 1.0;

/// How far in front of the player a dropped item lands (m).
constexpr double kDropAhead = 0.8;

/// The radius of the built-in sphere at scale 1, so a dropped one rests on the
/// ground rather than in it (m).
constexpr double kSphereRadius = 0.5;

/// The way a character faces at no rotation.
constexpr glm::dvec3 kForward{0.0, 0.0, -1.0};

double AcrossTheGround(const glm::dvec3 &a, const glm::dvec3 &b)
{
    return glm::length(glm::dvec2(a.x - b.x, a.z - b.z));
}

/// Where @p entity is drawn, or its Transform's position when it has no
/// world matrix yet.
glm::dvec3 WorldPositionOf(Assisi::ECS::Scene &scene, Entity entity)
{
    if (const Assisi::ECS::WorldMatrix *world = scene.Get<Assisi::ECS::WorldMatrix>(entity))
    {
        return Assisi::ECS::WorldPositionOf(*world);
    }
    return scene.Get<Assisi::ECS::Transform>(entity)->position;
}

/// Lays @p item free at @p where, as it lies when nobody holds it.
void LayFree(Assisi::ECS::Scene &scene, Entity item, const glm::dvec3 &where)
{
    Assisi::ECS::Transform *transform = scene.GetMut<Assisi::ECS::Transform>(item);
    transform->position = where;
    transform->rotation = glm::quat(1.f, 0.f, 0.f, 0.f);
}

/// The skinned body parented to @p character, or NullEntity.
Entity BodyOf(Assisi::ECS::Scene &scene, Entity character)
{
    for (auto [body, parent, skinned] : scene.Query<Assisi::ECS::Parent, Assisi::Runtime::SkinnedMesh>())
    {
        (void)skinned;
        if (parent.parent == character)
        {
            return body;
        }
    }
    return NullEntity;
}

/// Gives free @p item to the first player within reach of it, unless that is
/// the player who dropped it and hasn't walked away yet.
void OfferToPlayers(Assisi::ECS::Scene &scene, Entity item, const Holdable &holdable)
{
    const glm::dvec3 at = WorldPositionOf(scene, item);
    for (auto [player, tag, transform] : scene.Query<Assisi::App::Player, Assisi::ECS::Transform>())
    {
        (void)tag;
        const double distance = AcrossTheGround(at, transform.position);
        if (player == holdable.droppedBy)
        {
            if (distance > kWalkAway)
            {
                scene.GetMut<Holdable>(item)->droppedBy = NullEntity;
            }
            continue;
        }
        if (distance < kReach)
        {
            scene.GetMut<Holdable>(item)->holder = player;
            Assisi::ECS::Transform *held = scene.GetMut<Assisi::ECS::Transform>(item);
            held->position = glm::dvec3(holdable.offset);
            held->rotation = glm::quat(1.f, 0.f, 0.f, 0.f);
            return;
        }
    }
}

/// Where an item @p holder drops lands: on the ground in front of them.
glm::dvec3 DropPoint(Assisi::ECS::Scene &scene, Entity holder, Entity item)
{
    const Assisi::ECS::Transform &feet = *scene.Get<Assisi::ECS::Transform>(holder);
    const glm::dvec3 ahead = glm::mat3_cast(glm::dquat(feet.rotation)) * kForward * kDropAhead;
    const double radius = kSphereRadius * static_cast<double>(scene.Get<Assisi::ECS::Transform>(item)->scale.y);
    return feet.position + ahead + glm::dvec3(0.0, radius, 0.0);
}

/// Hangs @p item from @p body at @p joint, changing only what differs.
void Hang(Assisi::ECS::Scene &scene, Entity item, Entity body, const Assisi::Core::InternedString &joint)
{
    if (const Assisi::ECS::Parent *parent = scene.Get<Assisi::ECS::Parent>(item); parent == nullptr)
    {
        (void)scene.Add(item, Assisi::ECS::Parent{.parent = body});
    }
    else if (parent->parent != body)
    {
        scene.GetMut<Assisi::ECS::Parent>(item)->parent = body;
    }
    if (const Assisi::Runtime::JointAttachment *held = scene.Get<Assisi::Runtime::JointAttachment>(item);
        held == nullptr)
    {
        (void)scene.Add(item, Assisi::Runtime::JointAttachment{.joint = joint});
    }
    else if (held->joint != joint)
    {
        scene.GetMut<Assisi::Runtime::JointAttachment>(item)->joint = joint;
    }
}

/// Lets go of @p item: it stands in the world again where its Transform says.
void LetGo(Assisi::ECS::Scene &scene, Entity item)
{
    (void)scene.Remove<Assisi::Runtime::JointAttachment>(item);
    (void)scene.Remove<Assisi::ECS::Parent>(item);
}

} // namespace

void PickUpHoldablesSystem(Assisi::App::SystemContext &ctx)
{
    if (!ctx.net.IsAuthority())
    {
        return;
    }
    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (auto [item, holdable] : scene.Query<Holdable>())
    {
        if (holdable.holder == NullEntity)
        {
            OfferToPlayers(scene, item, holdable);
        }
        else if (!scene.IsAlive(holdable.holder))
        {
            // Its holder left: it falls where it was.
            const glm::dvec3 where = WorldPositionOf(scene, item);
            scene.GetMut<Holdable>(item)->holder = NullEntity;
            LayFree(scene, item, where);
        }
    }
}

void AskToDropSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr || !ctx.input->IsKeyPressed(Assisi::Window::Key::Q))
    {
        return;
    }
    for (auto [player, local] : ctx.world.scene.Query<Assisi::App::LocalPlayer>())
    {
        (void)local;
        (void)ctx.net.SendIntent(DropHeld{.dropper = player});
    }
}

void DropHoldablesSystem(Assisi::App::SystemContext &ctx)
{
    if (!ctx.net.IsAuthority())
    {
        return;
    }
    Assisi::ECS::Scene &scene = ctx.world.scene;
    for (const Assisi::Core::Received<DropHeld> &received : ctx.events.Read<Assisi::Core::Received<DropHeld>>())
    {
        const Entity dropper = received.message.dropper;
        for (auto [item, holdable] : scene.Query<Holdable>())
        {
            if (dropper == NullEntity || holdable.holder != dropper || !scene.IsAlive(dropper))
            {
                continue;
            }
            const glm::dvec3 where = DropPoint(scene, dropper, item);
            Holdable *dropped = scene.GetMut<Holdable>(item);
            dropped->holder = NullEntity;
            dropped->droppedBy = dropper;
            LayFree(scene, item, where);
        }
    }
}

void HoldHoldablesSystem(Assisi::App::SystemContext &ctx)
{
    Assisi::ECS::Scene &scene = ctx.world.scene;
    // Changed after the walk, since adding and removing components while
    // walking them is not safe.
    struct Hold
    {
        Entity item;
        Entity body;
        Assisi::Core::InternedString joint;
    };
    std::vector<Hold> held;
    std::vector<Entity> loose;
    for (auto [item, holdable] : scene.Query<Holdable>())
    {
        const bool holderHere = holdable.holder != NullEntity && scene.IsAlive(holdable.holder);
        const Entity body = holderHere ? BodyOf(scene, holdable.holder) : NullEntity;
        if (body != NullEntity)
        {
            held.push_back(Hold{.item = item, .body = body, .joint = holdable.joint});
        }
        else if (holderHere)
        {
            // Held by a character whose body hasn't arrived yet: left as it
            // is until it does, rather than shown at its offset from the
            // world's origin.
            continue;
        }
        else if (scene.Has<Assisi::Runtime::JointAttachment>(item) || scene.Has<Assisi::ECS::Parent>(item))
        {
            loose.push_back(item);
        }
    }
    for (const Hold &hold : held)
    {
        Hang(scene, hold.item, hold.body, hold.joint);
    }
    for (const Entity item : loose)
    {
        LetGo(scene, item);
    }
}

} // namespace Game
