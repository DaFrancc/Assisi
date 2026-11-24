/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestSystems.hpp
/// @brief Systems that exist to be named by a test level.
///
/// Declared with ASYSTEM rather than registered by hand, because that *is* the
/// thing under test: a declaration in a reflected header reaches the catalog by
/// being linked, and a level naming it gets it. Nothing calls a registration
/// function, which is the point — there is none to forget.
///
/// Defined inline, deliberately. assisi_link_reflections sweeps *every* generated
/// object into each final executable, so a header whose registrations reference
/// out-of-line functions would leave the game binaries with undefined symbols
/// for test code it has no reason to contain. Header-only keeps the registration
/// self-contained, which is the same reason the NetSync test components are.

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::App::Test
{

/// How many times each system has run, per world, so a test can tell "installed
/// and running" from "installed". Keyed by world address rather than counted
/// globally, because one instance running over two worlds is exactly the bug the
/// per-world registry exists to prevent.
class RunCounts
{
  public:
    static RunCounts &Instance()
    {
        static RunCounts counts;
        return counts;
    }

    void Reset() { _entries.clear(); }

    [[nodiscard]] std::uint32_t Count(const World &world, std::string_view system) const
    {
        for (const Entry &entry : _entries)
        {
            if (entry.world == &world && entry.system == system)
                return entry.runs;
        }
        return 0;
    }

    void Record(const World &world, std::string_view system)
    {
        for (Entry &entry : _entries)
        {
            if (entry.world == &world && entry.system == system)
            {
                ++entry.runs;
                return;
            }
        }
        _entries.push_back(Entry{.world = &world, .system = std::string{system}, .runs = 1});
    }

  private:
    struct Entry
    {
        const World *world = nullptr;
        std::string system;
        std::uint32_t runs = 0;
    };
    std::vector<Entry> _entries;
};

/// The sequence systems ran in, across every world.
///
/// Counts cannot answer an ordering question: two systems that each ran once say
/// nothing about which went first. Separate from RunCounts rather than folded
/// into it, because a per-world tally and a flat sequence are different shapes
/// and one container doing both serves neither well.
class RunOrder
{
  public:
    static RunOrder &Instance()
    {
        static RunOrder order;
        return order;
    }

    void Reset() { _names.clear(); }

    void Record(std::string_view name) { _names.emplace_back(name); }

    [[nodiscard]] const std::vector<std::string> &Names() const { return _names; }

  private:
    std::vector<std::string> _names;
};

ASYSTEM(Update, name = "Counter") inline void CounterSystem(SystemContext &ctx);

/// Ordered after Counter, so a test can assert the graph was honoured rather
/// than that both merely ran.
ASYSTEM(Update, name = "Follower", after = "Counter") inline void FollowerSystem(SystemContext &ctx);

/// One InputContext, N resident worlds — this is the flag that keeps a system
/// from applying the same keypresses in every one.
ASYSTEM(Update, name = "ActiveOnly", activeWorldOnly) inline void ActiveOnlySystem(SystemContext &ctx);

/// Turns contact reporting on for the world it runs in, which is what "the
/// system's own needs travel with the system" means in practice: a level that
/// names it gets the reporting too, without knowing it had to ask.
ASYSTEM(FixedUpdate, name = "Contacts") inline void ContactsSystem(SystemContext &ctx);

/// The one-shot phases, counted like the rest: a phase that fires twice and one
/// that fires once produce the same world, so only the tally tells them apart.
ASYSTEM(Begin, name = "Started") inline void StartedSystem(SystemContext &ctx);

/// Ordered after Started, because a one-shot phase sorts its entries too and an
/// unsorted walk would settle the order by registration instead.
ASYSTEM(Begin, name = "StartedLate", after = "Started") inline void StartedLateSystem(SystemContext &ctx);

ASYSTEM(Loaded, name = "Settled") inline void SettledSystem(SystemContext &ctx);

/// The asset store the last run of "SeesAssets" was handed, so a test can tell
/// whether the host's services reached a starting world's systems.
inline Core::AssetStore *&SeenAssets()
{
    static Core::AssetStore *seen = nullptr;
    return seen;
}

ASYSTEM(Begin, name = "SeesAssets") inline void SeesAssetsSystem(SystemContext &ctx);

/// A pair ordered against each other in the Render phase.
///
/// Render systems reach a world through a different SystemRegistry call than
/// every other phase, so their `after`/`before` travels a path of its own and
/// needs a case of its own — the Update pair above exercises none of it.
ASYSTEM(Render, name = "DrawEarly") inline void DrawEarlySystem(RenderContext &ctx);
ASYSTEM(Render, name = "DrawLate", after = "DrawEarly") inline void DrawLateSystem(RenderContext &ctx);

/// Drives every character with controls the way a game's player code does, so
/// the session tests can play characters without depending on any game's or
/// template's: turning turns the character about world up, looking up tilts
/// every camera parented to it, and the move, crouch, sprint and jump fill in
/// its CharacterIntent. `predicted`, so a client's replay steps it again from
/// the controls it recorded, as a game's own movement is.
ASYSTEM(FixedUpdate, name = "TestDrive", predicted, after = "LocalPlayer")
inline void TestDriveSystem(SystemContext &ctx);

inline void CounterSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Counter");
}

inline void FollowerSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Follower");
}

inline void ActiveOnlySystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "ActiveOnly");
}

inline void ContactsSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Contacts");
}

inline void StartedSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Started");
    RunOrder::Instance().Record("Started");
}

inline void StartedLateSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "StartedLate");
    RunOrder::Instance().Record("StartedLate");
}

inline void SettledSystem(SystemContext &ctx)
{
    RunCounts::Instance().Record(ctx.world, "Settled");
}

inline void SeesAssetsSystem(SystemContext &ctx)
{
    SeenAssets() = ctx.assets;
}

inline void DrawEarlySystem(RenderContext &)
{
    RunOrder::Instance().Record("DrawEarly");
}

inline void DrawLateSystem(RenderContext &)
{
    RunOrder::Instance().Record("DrawLate");
}

inline void TestDriveSystem(SystemContext &ctx)
{
    constexpr float kMaxPitch = glm::radians(89.f);
    ECS::Scene &scene = ctx.world.scene;

    // Collected first: turning writes Transforms while a query over the
    // controls walks, and tilting walks another query inside.
    std::vector<ECS::Entity> driven;
    for (auto [entity, controls, intent] : scene.Query<PlayerInput, Physics::CharacterIntent>())
    {
        (void)controls;
        (void)intent;
        driven.push_back(entity);
    }

    for (const ECS::Entity entity : driven)
    {
        const PlayerInput controls = *scene.Get<PlayerInput>(entity);
        ECS::Transform *body = scene.GetMut<ECS::Transform>(entity);
        if (body != nullptr && controls.lookYaw != 0.f)
        {
            body->rotation =
                glm::normalize(glm::angleAxis(controls.lookYaw, glm::vec3(0.f, 1.f, 0.f)) * body->rotation);
        }
        if (controls.lookPitch != 0.f)
        {
            for (auto [child, camera, parent] : scene.Query<Runtime::Camera, ECS::Parent>())
            {
                (void)camera;
                ECS::Transform *eye = parent.parent == entity ? scene.GetMut<ECS::Transform>(child) : nullptr;
                if (eye == nullptr)
                {
                    continue;
                }
                const float current = std::asin(glm::clamp((eye->rotation * glm::vec3(0.f, 0.f, -1.f)).y, -1.f, 1.f));
                const float pitch = glm::clamp(current + controls.lookPitch, -kMaxPitch, kMaxPitch);
                eye->rotation = glm::normalize(glm::angleAxis(pitch, glm::vec3(1.f, 0.f, 0.f)));
            }
        }

        glm::vec3 move{controls.move.x, 0.f, -controls.move.y};
        if (body != nullptr)
        {
            move = body->rotation * move;
        }
        move.y = 0.f;
        const float length = glm::min(glm::length(controls.move), 1.f);
        Physics::CharacterIntent &intent = *scene.GetMut<Physics::CharacterIntent>(entity);
        intent.move = length > 0.f && glm::dot(move, move) > 0.f ? glm::normalize(move) * length : glm::vec3{0.f};
        intent.stance =
            controls.held.Has(PlayerAction::Crouch) ? Physics::Stance::Crouching : Physics::Stance::Standing;
        intent.sprint = controls.held.Has(PlayerAction::Sprint);
        intent.jump = intent.jump || controls.pressed.Has(PlayerAction::Jump);
    }
}

} // namespace Assisi::App::Test
