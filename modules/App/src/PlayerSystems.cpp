/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PlayerSystems.hpp>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Reflect/EnumLabels.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <Assisi/NetSync/NetComponents.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

namespace Assisi::App
{
namespace
{

/// The name reflection knows PlayerAction by, which is what the game's names
/// are registered under.
constexpr std::string_view kPlayerActionEnumType = "Assisi::App::PlayerAction";

/// Radians of turn per pixel of mouse movement.
const float kLookRadiansPerPixel = glm::radians(kLookDegreesPerPixel);

/// How far a world's removals of LocalPlayer have been read, so each is
/// announced once.
struct LocalPlayerCursor
{
    uint64_t tick = 0;
};

/// Whether @p entity is this machine's player, by the world's LocalClient.
bool IsLocalPlayer(const ECS::Scene &scene, ECS::Entity entity, const LocalClient &local)
{
    if (!local.networked)
    {
        return true;
    }
    const NetSync::ControlledBy *claim = scene.Get<NetSync::ControlledBy>(entity);
    return local.id != 0 && claim != nullptr && claim->client == local.id;
}

/// The movement keys as a direction in the character's frame: x to the right,
/// y forward.
glm::vec2 MoveFromKeys(const Window::ActionMap &actions, const Window::InputContext &input)
{
    glm::vec2 move{0.f, 0.f};
    if (actions.IsActionDown(kActionMoveForward, input))
    {
        move.y += 1.f;
    }
    if (actions.IsActionDown(kActionMoveBackward, input))
    {
        move.y -= 1.f;
    }
    if (actions.IsActionDown(kActionMoveLeft, input))
    {
        move.x -= 1.f;
    }
    if (actions.IsActionDown(kActionMoveRight, input))
    {
        move.x += 1.f;
    }

    // Normalized, or holding two keys would walk faster diagonally than straight.
    // Guarded because normalizing a zero vector is a division by zero that poisons
    // every downstream number with NaN.
    if (glm::dot(move, move) > 0.f)
    {
        move = glm::normalize(move);
    }
    return move;
}

/// Tilts every camera parented to @p character up by @p radians, within
/// kMaxPitchDegrees of level.
void PitchCameras(ECS::Scene &scene, ECS::Entity character, float radians)
{
    for (auto [child, camera, parent] : scene.Query<Runtime::Camera, ECS::Parent>())
    {
        (void)camera;
        if (parent.parent != character)
        {
            continue;
        }
        ECS::Transform *transform = scene.GetMut<ECS::Transform>(child);
        if (transform == nullptr)
        {
            continue;
        }
        transform->rotation = Tilted(transform->rotation, radians);
    }
}

/// Turns @p character left by @p radians.
void TurnCharacter(ECS::Scene &scene, ECS::Entity character, float radians)
{
    ECS::Transform *transform = scene.GetMut<ECS::Transform>(character);
    if (transform == nullptr)
    {
        return;
    }
    transform->rotation = Turned(transform->rotation, radians);
}

/// @p move, in the character's frame, as a world direction along the ground of
/// the same length: looking down must not walk a character into the floor.
glm::vec3 WorldMove(const ECS::Scene &scene, ECS::Entity character, glm::vec2 move)
{
    const float length = glm::min(glm::length(move), 1.f);
    if (length <= 0.f)
    {
        return glm::vec3{0.f};
    }
    glm::vec3 world{move.x, 0.f, -move.y};
    if (const ECS::Transform *transform = scene.Get<ECS::Transform>(character))
    {
        world = transform->rotation * world;
    }
    world.y = 0.f;
    if (glm::dot(world, world) <= 0.f)
    {
        return glm::vec3{0.f};
    }
    return glm::normalize(world) * length;
}

} // namespace

glm::quat Turned(const glm::quat &facing, float radians)
{
    return glm::normalize(glm::angleAxis(radians, glm::vec3(0.f, 1.f, 0.f)) * facing);
}

glm::quat Tilted(const glm::quat &eye, float radians)
{
    // Rebuilt from an accumulated angle rather than multiplied in, so the clamp
    // is a clamp: composing a delta each step and then limiting the result lets
    // it creep past the limit and stick there.
    const glm::vec3 forward = eye * glm::vec3(0.f, 0.f, -1.f);
    const float current = std::asin(glm::clamp(forward.y, -1.f, 1.f));
    const float limit = glm::radians(kMaxPitchDegrees);
    const float pitch = glm::clamp(current + radians, -limit, limit);
    return glm::normalize(glm::angleAxis(pitch, glm::vec3(1.f, 0.f, 0.f)));
}

std::vector<std::string> PlayerActionNames()
{
    std::vector<std::string> names(static_cast<std::size_t>(PlayerAction::Count_));
    names[static_cast<std::size_t>(PlayerAction::Jump)] = kActionJump;
    names[static_cast<std::size_t>(PlayerAction::Crouch)] = kActionCrouch;
    names[static_cast<std::size_t>(PlayerAction::Sprint)] = kActionSprint;
    for (const Core::Reflect::EnumConstant &label : Core::Reflect::EnumLabelsOf(kPlayerActionEnumType))
    {
        if (label.value >= 0 && label.value < static_cast<std::int64_t>(names.size()))
        {
            names[static_cast<std::size_t>(label.value)] = label.name;
        }
    }
    return names;
}

PlayerActionRegistration::PlayerActionRegistration(std::span<const PlayerActionName> names)
{
    for (const PlayerActionName &entry : names)
    {
        Core::Reflect::RegisterEnumLabel(kPlayerActionEnumType, static_cast<std::int64_t>(entry.action), entry.name);
    }
}

void LocalPlayerSystem(SystemContext &ctx)
{
    ECS::Scene &scene = ctx.world.scene;
    const LocalClient *found = ctx.world.resources.Find<LocalClient>();
    const LocalClient local = found != nullptr ? *found : LocalClient{};

    // A destroyed player leaves no Player to walk below, so the scene's log of
    // removals is what tells of it. A log that no longer reaches this far back
    // has lost a removal only after thousands of them in one step, or a cleared
    // scene, and nothing is left to ask either way.
    uint64_t &seen = ctx.world.resources.Get<LocalPlayerCursor>().tick;
    std::vector<ECS::Entity> removed;
    (void)scene.RemovedSince<LocalPlayer>(seen, removed);
    for (const ECS::Entity entity : removed)
    {
        if (!scene.IsAlive(entity))
        {
            ctx.events.Push(LocalPlayerLost{.entity = entity});
        }
    }

    // Collected first: adding and removing components while a query walks them
    // can move what it is walking.
    std::vector<ECS::Entity> gained;
    std::vector<ECS::Entity> lost;
    for (auto [entity, player] : scene.Query<Player>())
    {
        (void)player;
        const bool mine = IsLocalPlayer(scene, entity, local);
        const bool tagged = scene.Has<LocalPlayer>(entity);
        if (mine && !tagged)
        {
            gained.push_back(entity);
        }
        else if (!mine && tagged)
        {
            lost.push_back(entity);
        }
    }

    for (const ECS::Entity entity : gained)
    {
        (void)scene.Add<LocalPlayer>(entity);
        if (!scene.Has<PlayerInput>(entity))
        {
            (void)scene.Add<PlayerInput>(entity);
        }
        ctx.events.Push(LocalPlayerGained{.entity = entity});
    }
    for (const ECS::Entity entity : lost)
    {
        (void)scene.Remove<LocalPlayer>(entity);
        (void)scene.Remove<PlayerInput>(entity);
        ctx.events.Push(LocalPlayerLost{.entity = entity});
    }
    // After the removals above, which have been announced already.
    seen = scene.CurrentChangeTick();
}

void PlayerGatherSystem(SystemContext &ctx)
{
    if (ctx.input == nullptr || ctx.actions == nullptr)
    {
        return;
    }
    const Window::InputContext &input = *ctx.input;
    const Window::ActionMap &actions = *ctx.actions;

    // Taken once: the game's names are registered before anything runs, and
    // nothing adds to them after.
    static const std::vector<std::string> names = PlayerActionNames();

    PlayerButtons held;
    PlayerButtons pressed;
    for (std::size_t bit = 0; bit < names.size(); ++bit)
    {
        if (names[bit].empty())
        {
            continue;
        }
        const PlayerAction action = static_cast<PlayerAction>(bit);
        if (actions.IsActionDown(names[bit], input))
        {
            held = held.With(action);
        }
        if (actions.IsActionPressed(names[bit], input))
        {
            pressed = pressed.With(action);
        }
    }

    const glm::vec2 move = MoveFromKeys(actions, input);
    const glm::vec2 turn = input.IsMouseCaptured() ? input.MouseDelta() * kLookRadiansPerPixel : glm::vec2{0.f};

    for (auto [entity, controls, local] : ctx.world.scene.Query<Mut<PlayerInput>, LocalPlayer>())
    {
        (void)local;
        controls.move = move;
        controls.held = held;
        // Added up rather than set: a frame that runs no step must not lose a
        // press or a turn, and the step clears them once it has used them.
        controls.pressed = PlayerButtons{static_cast<std::uint32_t>(controls.pressed.bits | pressed.bits)};
        controls.lookYaw -= turn.x;
        controls.lookPitch -= turn.y;
    }
}

void CharacterCommandSystem(SystemContext &ctx)
{
    ECS::Scene &scene = ctx.world.scene;

    // Collected first: turning writes Transforms, which a query over these
    // components does not walk, but pitching walks another query inside.
    std::vector<ECS::Entity> commanded;
    for (auto [entity, controls, intent] : scene.Query<PlayerInput, Physics::CharacterIntent>())
    {
        (void)controls;
        (void)intent;
        commanded.push_back(entity);
    }

    for (const ECS::Entity entity : commanded)
    {
        PlayerInput &controls = *scene.GetMut<PlayerInput>(entity);
        if (controls.lookYaw != 0.f)
        {
            TurnCharacter(scene, entity, controls.lookYaw);
        }
        if (controls.lookPitch != 0.f)
        {
            PitchCameras(scene, entity, controls.lookPitch);
        }

        Physics::CharacterIntent &intent = *scene.GetMut<Physics::CharacterIntent>(entity);
        intent.move = WorldMove(scene, entity, controls.move);
        intent.stance =
            controls.held.Has(PlayerAction::Crouch) ? Physics::Stance::Crouching : Physics::Stance::Standing;
        intent.sprint = controls.held.Has(PlayerAction::Sprint);
        // OR rather than assign: the step clears the request once it has taken
        // it, and a press must survive steps that could not jump yet.
        intent.jump = intent.jump || controls.pressed.Has(PlayerAction::Jump);

        controls.lookYaw = 0.f;
        controls.lookPitch = 0.f;
        controls.pressed = PlayerButtons{};
    }
}

void PlayerCameraSystem(SystemContext &ctx)
{
    ECS::Scene &scene = ctx.world.scene;
    for (auto [child, camera, parent] : scene.Query<Runtime::Camera, ECS::Parent>())
    {
        if (!scene.Has<Player>(parent.parent))
        {
            continue;
        }
        const bool active = scene.Has<LocalPlayer>(parent.parent);
        // Compared first: a write stamps the camera as changed every frame.
        if (camera.isActive != active)
        {
            scene.GetMut<Runtime::Camera>(child)->isActive = active;
        }
    }
}

} // namespace Assisi::App
