/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/App/PlayerSystems.hpp>

#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/EnumLabels.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Window/ActionMap.hpp>
#include <Assisi/Window/InputContext.hpp>

#include <Assisi/NetSync/NetComponents.hpp>

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

} // namespace

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

void ClearUsedControls(ECS::Scene &scene)
{
    for (auto [entity, controls] : scene.Query<Mut<PlayerInput>>())
    {
        (void)entity;
        controls.lookYaw = 0.f;
        controls.lookPitch = 0.f;
        controls.pressed = PlayerButtons{};
    }
}

void CaptureCursorSystem(SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no window to capture into
    {
        return;
    }
    ctx.input->SetInputMode(Window::InputMode::Game);
    Core::Log::Info("CaptureCursor: '{}' takes the mouse.", ctx.world.levelPath);
}

void CursorToggleSystem(SystemContext &ctx)
{
    if (ctx.input == nullptr) // headless host: no window, no cursor to move
    {
        return;
    }
    // Only while the cursor is free, and never while a screen has it: a click
    // meant for a menu button must not also recapture the pointer.
    if (ctx.input->IsMouseCaptured() || (ctx.ui != nullptr && ctx.ui->TakesInput()))
    {
        return;
    }
    if (ctx.input->IsMouseButtonPressed(Window::MouseButton::Left))
    {
        ctx.input->SetInputMode(Window::InputMode::Game);
    }
}

} // namespace Assisi::App
