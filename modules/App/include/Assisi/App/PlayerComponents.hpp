/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PlayerComponents.hpp
/// @brief A player's controls as a component, which entity is this machine's
///        player, and where players start.
///
/// A player's controls are the same thing on every machine: the one whose
/// keyboard it is fills them in from the devices, and a host fills them in for a
/// remote player from what that player's machine sends. Everything that moves a
/// character reads them, and never knows which way they came.

#include <Assisi/Core/AssetPath.hpp>
#include <Assisi/Core/Bitmask.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::App
{

/// @brief A button a player presses, as one bit of their controls.
///
/// The enumerators are the engine's own, which a character's movement reads. The
/// slots between the last of them and `Count_` belong to the game, which takes
/// them with GameAction and names them with ASSISI_PLAYER_ACTIONS.
///
/// An enumerator's value is its bit on the wire, so each is spelled out:
/// renumbering one makes two builds read each other's buttons as different
/// ones.
AENUM()
enum class PlayerAction : std::uint8_t
{
    Jump = 0,
    Crouch = 1,
    Sprint = 2,

    /// One past the last slot: the controls have a bit for every action, the
    /// game's included.
    Count_ = 32,
};

static_assert(static_cast<std::uint32_t>(PlayerAction::Count_) == Core::kBitmaskWidth<std::uint32_t>,
              "Every bit of a player's buttons is an action, and every action has a bit.");

/// The buttons a player holds or pressed, one bit per PlayerAction.
using PlayerButtons = Core::Bitmask<PlayerAction, std::uint32_t>;

/// The first slot after the engine's own actions.
inline constexpr std::uint32_t kFirstGameAction = static_cast<std::uint32_t>(PlayerAction::Sprint) + 1u;

/// How many actions a game may take.
inline constexpr std::uint32_t kGameActionCount =
    static_cast<std::uint32_t>(PlayerAction::Count_) - kFirstGameAction;

/// Deliberately not constexpr: GameAction calls it only for an index out of
/// range, and calling it while evaluating a consteval function is what turns that
/// index into a build error naming this function.
void GameActionIndexIsOutOfRange();

/// @brief The game's action @p index, from 0 to kGameActionCount - 1.
consteval PlayerAction GameAction(std::uint32_t index)
{
    if (index >= kGameActionCount)
    {
        GameActionIndexIsOutOfRange();
    }
    return static_cast<PlayerAction>(kFirstGameAction + index);
}

/// The input actions the engine's own player actions read, by name in the
/// input bindings.
inline constexpr std::string_view kActionJump = "Jump";
inline constexpr std::string_view kActionCrouch = "Crouch";
inline constexpr std::string_view kActionSprint = "Sprint";
inline constexpr std::string_view kActionMoveForward = "MoveForward";
inline constexpr std::string_view kActionMoveBackward = "MoveBackward";
inline constexpr std::string_view kActionMoveLeft = "MoveLeft";
inline constexpr std::string_view kActionMoveRight = "MoveRight";

/// One game action and the input action that presses it, by its name in the
/// input bindings.
struct PlayerActionName
{
    const char *name = "";
    PlayerAction action = PlayerAction::Jump;
};

/// @brief Whether @p names can all stand: each names a game slot rather than one
/// of the engine's own actions, has a name, and shares neither its slot nor its
/// name with another.
consteval bool ValidPlayerActionNames(std::span<const PlayerActionName> names)
{
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (static_cast<std::uint32_t>(names[i].action) < kFirstGameAction || std::string_view(names[i].name).empty())
        {
            return false;
        }
        for (std::size_t j = i + 1; j < names.size(); ++j)
        {
            if (names[i].action == names[j].action || std::string_view(names[i].name) == names[j].name)
            {
                return false;
            }
        }
    }
    return true;
}

/// @brief Hands the game's action names to the engine when constructed. Made by
/// ASSISI_PLAYER_ACTIONS rather than by hand, so the table is always checked
/// first.
class PlayerActionRegistration
{
public:
    explicit PlayerActionRegistration(std::span<const PlayerActionName> names);
};

/// @brief The input action each PlayerAction reads, entry N for bit N: the
/// engine's own, then the game's, and an empty name for a slot nobody took.
///
/// What both ends of a network session agree on before either reads the
/// other's buttons.
[[nodiscard]] std::vector<std::string> PlayerActionNames();

/// @brief A player's controls for the next fixed step.
///
/// Movement and held buttons are the state of the controls now. Turning and
/// presses are what happened since the last fixed step, so a frame that runs no
/// step loses neither: they add up until a step uses them and clears them.
///
/// Never saved or sent as it is: the machine whose player it is fills it in, and
/// a host fills in a remote player's from the commands that player sends.
ACOMP(transient)
struct PlayerInput
{
    /// Where the player is asking to go, in the character's own frame: x to the
    /// right, y forward. No longer than 1.
    AFIELD() glm::vec2 move{0.f, 0.f};

    /// Radians to turn left since the last fixed step.
    AFIELD() float lookYaw = 0.f;

    /// Radians to look up since the last fixed step.
    AFIELD() float lookPitch = 0.f;

    /// The buttons held now.
    AFIELD() PlayerButtons held;

    /// The buttons pressed since the last fixed step, held or since let go.
    AFIELD() PlayerButtons pressed;
};

/// @brief Marks a character a player can control, as against one an AI or a
/// script moves.
///
/// Authored, in the player's blueprint. With no network session the player
/// characters are this machine's player; in a session the host hands one to
/// each player that joins.
ACOMP(replicable, requires = {Character})
struct Player
{
};

/// @brief Marks a character as this machine's player: the one its keyboard and
/// mouse drive and its camera looks through.
///
/// Written by the engine every fixed step, never by hand. With no network
/// session every Player is this machine's; in a session, the one this machine
/// controls.
ACOMP(transient)
struct LocalPlayer
{
};

/// @brief A character has just become this machine's player: what anything
/// made per player, such as a HUD, is made on. Pushed by the step that tags
/// it LocalPlayer.
AEVENT()
struct LocalPlayerGained
{
    ECS::Entity entity = ECS::NullEntity;
};

/// @brief A character has just stopped being this machine's player while
/// still in the world. One destroyed while it was the player sends nothing,
/// since nothing is left to tell anyone about.
AEVENT()
struct LocalPlayerLost
{
    ECS::Entity entity = ECS::NullEntity;
};

/// @brief Where a player's character is made when one joins a networked game,
/// and from which blueprint.
///
/// The host gives each player that joins, and itself, a character: the first
/// unclaimed Player the level already holds, or a new one of `blueprint` here.
/// With several starts, players are spread over them in the order they joined.
ACOMP()
struct PlayerStart
{
    AFIELD() Assisi::Core::AssetPath blueprint{std::string_view{"blueprints/Player.abp"}};
};

/// @brief Which network client this machine is, for the world it plays. A
/// resource of the world, not a component.
///
/// Written each fixed step by whatever runs the network session.
struct LocalClient
{
    /// The `ControlledBy` client id that is this machine's: 1 for a host,
    /// what the host assigned for a client. 0 while it has none yet.
    std::uint32_t id = 0;

    /// Whether the world is played in a network session at all. Off, every
    /// Player is this machine's.
    bool networked = false;
};

} // namespace Assisi::App

/// @brief Checks the game's action table @p table at compile time and registers
/// its names.
///
/// @code
/// inline constexpr Assisi::App::PlayerAction Aim = Assisi::App::GameAction(0);
///
/// inline constexpr Assisi::App::PlayerActionName kPlayerActions[] = {
///     {"Aim", Aim},
/// };
/// ASSISI_PLAYER_ACTIONS(kPlayerActions);
/// @endcode
///
/// Belongs in a header under the game's src/. Every header there is compiled
/// into both the game and the editor through their reflection, which is what
/// keeps this registration from being dropped as unreferenced.
#define ASSISI_PLAYER_ACTIONS(table)                                                                                   \
        static_assert(::Assisi::App::ValidPlayerActionNames(table),                                                        \
                      "Each player action needs a name, a slot from GameAction, and neither shared with another.");       \
        inline const ::Assisi::App::PlayerActionRegistration table ## Registration { table }
