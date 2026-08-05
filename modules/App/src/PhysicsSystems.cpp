/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PhysicsSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <vector>

namespace Assisi::App
{

void BounceSystem(SystemContext &ctx)
{
    // Its own need, turned on where the need is rather than in whatever file
    // happened to name this system. Idempotent, so it also survives a world that
    // switched reporting off.
    if (!ctx.world.physics.IsContactReporting())
        ctx.world.physics.SetContactReporting(true);

    ECS::Scene &scene = ctx.world.scene;

    // Walk the contact log rather than querying for Bounce entities: contacts
    // number in the handful even when bouncers number in the thousands, and an
    // entity that touched nothing this step has nothing to do either way.
    const std::span<const Physics::Contact> contacts = ctx.world.physics.Contacts();
    if (contacts.empty())
        return;

    // One bounce per entity per step (see the header). Function-local, so nothing
    // carries between frames or between the worlds this same function serves —
    // a system installed into several worlds must keep its state in components,
    // never in the system itself.
    std::vector<ECS::Entity> bounced;

    for (const Physics::Contact &contact : contacts)
    {
        const Physics::Bounce *bounce = scene.Get<Physics::Bounce>(contact.entity);
        if (bounce == nullptr)
            continue;

        // The impact was logged against a live body during the last step; the
        // entity can still have been destroyed since, or had its collider removed.
        if (!scene.IsAlive(contact.entity))
            continue;
        const Physics::RigidBody *body = scene.Get<Physics::RigidBody>(contact.entity);
        if (body == nullptr)
            continue;

        // Only a body approaching hard enough bounces. Two things are being
        // rejected here, and both matter:
        //
        //  - Anything not moving into the surface (closing speed >= 0). Jolt's
        //    speculative contacts fire a little before a real touch and also for
        //    bodies already moving apart; reflecting one of those would drive it
        //    back into the surface it just left.
        //  - Anything slower than kMinBounceSpeed. That is the floor that lets a
        //    bouncy body come to rest — see the constant for why settling noise
        //    otherwise feeds itself.
        //
        // The measure is the closing speed, not the speed: a body skimming fast
        // along a floor is barely touching it, and should not be launched for it.
        const float closingSpeed = glm::dot(contact.velocity, contact.normal);
        if (closingSpeed > -kMinBounceSpeed)
            continue;

        if (std::find(bounced.begin(), bounced.end(), contact.entity) != bounced.end())
            continue;
        bounced.push_back(contact.entity);

        // Mirror the incoming velocity about the surface, then scale it. Reflection
        // rather than plain negation is what makes it read as a bouncy ball: a body
        // arriving at an angle leaves at the mirrored angle and keeps its sideways
        // travel, instead of retracing the path it came in on. For a head-on hit
        // the two are the same thing.
        //
        // `rebound` is a speed multiplier, which is what makes 0 / 1 / >1 behave as
        // "stops dead" / "loses nothing" / "gains on every hit". Negative is
        // clamped rather than trusted: the inspector floors the field, but a level
        // file is just text and can hold anything.
        const glm::vec3 reflected = contact.velocity - 2.f * closingSpeed * contact.normal;
        const float     rebound   = glm::max(bounce->rebound, 0.f);
        ctx.world.physics.SetBodyLinearVelocity(*body, reflected * rebound);
    }
}

} // namespace Assisi::App
