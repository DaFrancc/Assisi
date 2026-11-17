/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestPlayerSessionPlayerBodies.cpp
/// @brief Every player's body animates on a host and two clients from what its
///        character does, each machine working it out from the character
///        state it was sent.
///
/// The players' bodies run an animator shaped like the sample game's player,
/// on a skeleton built in code, fed from CharacterState the way the sample's
/// PlayerAnimation system feeds it. The sample's own system lives in the game,
/// which tests don't link, so this feeds the same params from the same state.

#include "PlayerSessionTesting.hpp"

#include "AnimatorTesting.hpp"
#include "TestRig.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <expected>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <Assisi/App/Network.hpp>
#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/TestComponents.hpp>
#include <Assisi/App/TestMessages.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Received.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/NetSync/NetComponents.hpp>
#include <Assisi/NetSync/ReplicationClient.hpp>
#include <Assisi/NetSync/ReplicationServer.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Render/MeshBuffer.hpp>
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/AnimatorParams.hpp>
#include <Assisi/Runtime/AnimatorStep.hpp>
#include <Assisi/Runtime/Components.hpp>

using namespace Assisi;
using namespace Assisi::App::Testing;

namespace
{

constexpr std::string_view kModelPath = "tests/biped.glb";
constexpr std::string_view kAnimatorPath = "tests/player.sgl";

/// Each state's clip, on the biped built in code.
constexpr std::array<std::string_view, 8> kClipPaths{"tests/idle.glb",  "tests/crouch_idle.glb",
                                                     "tests/crouch_walk.glb", "tests/jump.glb",
                                                     "tests/fall.glb",  "tests/land.glb",
                                                     "tests/interact.glb", "tests/walk.glb"};

/// The sample player's animator, with clips for its blend space.
constexpr std::string_view kAnimator = R"(use animation;
skeleton "tests/biped.glb";
param speed: float;
param airborne: bool;
param crouched: bool;
param jump: trigger;
param land: trigger;
param interact: trigger;
layer base {
    state locomotion { play "tests/walk.glb"; rate speed; }
    state crouch {
        state crouch_idle { play "tests/crouch_idle.glb"; }
        state crouch_walk { play "tests/crouch_walk.glb"; }
        crouch_idle -> crouch_walk when speed > 0.3;
        crouch_walk -> crouch_idle when speed < 0.15;
    }
    state jump { play "tests/jump.glb"; then fall; }
    state fall { play "tests/fall.glb"; }
    state land { play "tests/land.glb"; then locomotion; }
    state interact { play "tests/interact.glb"; then locomotion; }
    locomotion + crouch + interact + land -> jump when jump { fade 0.05; interrupt; };
    locomotion + crouch + interact -> fall when airborne;
    jump + fall -> land when land { fade 0.1; interrupt; };
    land -> fall when airborne { interrupt; };
    locomotion -> crouch when crouched { interrupt; };
    crouch -> locomotion when !crouched;
    locomotion + crouch -> interact when interact { interrupt; };
}
)";

/// How long each clip lasts (s): a jump's start shorter than any jump.
constexpr float kClipSeconds = 0.2f;

/// How far each clip swings a limb (degrees): only which state plays is
/// looked at, so any visible swing will do.
constexpr float kClipDegrees = 20.f;

/// What the GPU would have given the mesh; any id but the unbound one.
constexpr uint32_t kMeshId = 1;

/// As the sample's PlayerAnimation: how long speed eases (s), and how long a
/// character is off the ground before it counts as airborne (s).
constexpr float kSpeedEase = 0.1f;
constexpr float kStairFlicker = 0.1f;

/// Frames each player does its part for before every machine is looked at:
/// long enough to reach full speed or a crouch, and for every machine to hear.
constexpr std::int32_t kActFrames = 90;

/// Frames for a jump to go up, come down and land, on every machine.
constexpr std::int32_t kJumpFrames = 90;

/// A sprinting character's speed is above this share of its walk speed.
constexpr float kSprintingAtLeast = 1.2f;

/// A standing character's speed is below this (m/s).
constexpr float kStandingBelow = 0.2f;

/// Starts for the host and two clients (m), 4 m apart.
constexpr std::array<glm::dvec3, 3> kStarts{glm::dvec3{0.0}, glm::dvec3{4.0, 0.0, 0.0}, glm::dvec3{8.0, 0.0, 0.0}};

/// What every machine is given in place of files.
struct Character
{
    Render::MeshBuffer mesh;
    std::vector<std::shared_ptr<const Geometry::AnimationClip>> clips;
    std::shared_ptr<const Runtime::AnimatorGraph> animator;
};

void Build(Character &character)
{
    const Geometry::Skeleton skeleton = Runtime::Testing::BuildSkeleton(Runtime::Testing::TwoLimbs());
    Runtime::Testing::RigTree tree;
    tree.Model(std::string{kModelPath}, skeleton);
    for (const std::string_view path : kClipPaths)
    {
        tree.Clip(std::string{path});
        character.clips.push_back(
            Runtime::Testing::CycleClip("limb_l_0", glm::vec3(1.f, 0.f, 0.f), kClipDegrees, kClipSeconds));
    }
    std::expected<Runtime::AnimatorGraph, std::string> graph = Runtime::Import::CompileAnimator(kAnimator, tree);
    REQUIRE_MESSAGE(graph.has_value(), graph.error());
    character.animator = std::make_shared<const Runtime::AnimatorGraph>(std::move(*graph));
    character.mesh.KeepWithoutGpu(Runtime::Testing::BuildSkinnedBoxes(skeleton));
    character.mesh.SetId(kMeshId);
}

void Provide(Machine &machine, const Character &character)
{
    for (std::size_t clip = 0; clip < kClipPaths.size(); ++clip)
    {
        machine.assets.Provide(Core::DerivedAssetId(kClipPaths[clip]), character.clips[clip]);
    }
    machine.assets.Provide(Core::DerivedAssetId(kAnimatorPath), character.animator);
}

/// A reference to the file at @p path as a blueprint writes one.
nlohmann::json Reference(std::string_view path)
{
    return {{"guid", Core::DerivedAssetId(path).ToString()}, {"path", std::string{path}}};
}

nlohmann::json Placed()
{
    return {{"position", {0.f, 0.f, 0.f}}, {"rotation", {1.f, 0.f, 0.f, 0.f}}, {"scale", {1.f, 1.f, 1.f}}};
}

/// A player whose body, a child, is the skinned biped running the animator.
void WritePlayerWithBody(const std::filesystem::path &root)
{
    const nlohmann::json player = {{"Transform", Placed()},
        {"Character", nlohmann::json::object()},
        {"CharacterLocomotion", nlohmann::json::object()},
        {"ArcadeMovement", nlohmann::json::object()},
        {"Player", nlohmann::json::object()},
        {"Replicated", nlohmann::json::object()}};
    const nlohmann::json body = {{"Transform", Placed()},
        {"Parent", {{"parent", "Player"}}},
        {"MeshRenderer", {{"mesh", Reference(kModelPath)}}},
        {"SkinnedMesh", nlohmann::json::object()},
        {"AnimationPlayer", nlohmann::json::object()},
        {"Animator", {{"machine", Reference(kAnimatorPath)}}},
        {"Replicated", nlohmann::json::object()}};
    const nlohmann::json file = {{"version", 2},
        {"entities", nlohmann::json::array({{{"name", "Player"}, {"components", player}},
                                               {{"name", "Body"}, {"components", body}}})}};
    std::ofstream out(root / "player.abp", std::ios::binary);
    out << file.dump(2);
    REQUIRE(out.good());
}

/// Points every skinned entity on @p machine at the mesh, as the GPU cache would.
void BindMeshes(Machine &machine, const Character &character)
{
    for (auto [entity, skinned] : machine.world.scene.Query<Runtime::SkinnedMesh>())
    {
        (void)skinned;
        machine.world.scene.Get<Runtime::MeshRenderer>(entity)->meshBuffer = &character.mesh;
    }
}

/// Sets every player's body's params on @p machine from its character's state,
/// as the sample's PlayerAnimation system does.
void FeedPlayers(Machine &machine)
{
    static Runtime::AnimatorParam<float> speed{"speed"};
    static Runtime::AnimatorParam<bool> airborne{"airborne"};
    static Runtime::AnimatorParam<bool> crouched{"crouched"};
    static Runtime::AnimatorTrigger jump{"jump"};
    static Runtime::AnimatorTrigger land{"land"};
    ECS::Scene &scene = machine.world.scene;
    for (auto [body, run, parent] : scene.Query<Mut<Runtime::AnimatorRun>, ECS::Parent>())
    {
        const Physics::CharacterState *state = scene.Get<Physics::CharacterState>(parent.parent);
        if (state == nullptr || !scene.Has<App::Player>(parent.parent))
        {
            continue;
        }
        speed.SetDamped(body, run, state->groundSpeed, kSpeedEase, kStep);
        airborne.Set(body, run,
                     state->ground != Physics::GroundState::OnGround && state->timeSinceGrounded >= kStairFlicker);
        crouched.Set(body, run, state->stance == Physics::Stance::Crouching);
        jump.FireOnChange(body, run, state->jumps);
        land.FireOnChange(body, run, state->landings);
    }
}

/// The body parented to @p player on @p scene, or NullEntity.
ECS::Entity BodyOf(ECS::Scene &scene, ECS::Entity player)
{
    for (auto [body, run, parent] : scene.Query<Runtime::AnimatorRun, ECS::Parent>())
    {
        (void)run;
        if (parent.parent == player)
        {
            return body;
        }
    }
    return ECS::NullEntity;
}

/// @p machine's copy of the host's entity @p onHost.
ECS::Entity CopyOn(Machine &host, Machine &machine, ECS::Entity onHost)
{
    if (&machine == &host)
    {
        return onHost;
    }
    return machine.play.Session().Client()->EntityOf(host.play.Session().Server()->NetIdOf(onHost));
}

/// The states @p player's body's base layer is in on @p machine.
std::vector<std::string> StatesOf(Machine &host, Machine &machine, ECS::Entity playerOnHost)
{
    ECS::Scene &scene = machine.world.scene;
    const ECS::Entity body = BodyOf(scene, CopyOn(host, machine, playerOnHost));
    REQUIRE(body != ECS::NullEntity);
    return Runtime::AnimatorStateNames(*scene.Get<Runtime::AnimatorRun>(body), 0);
}

/// @p states as one line, for a failed check to show.
std::string Joined(const std::vector<std::string> &states)
{
    std::string line;
    for (const std::string &state : states)
    {
        line += (line.empty() ? "" : "/") + state;
    }
    return line;
}

/// The speed param of @p player's body on @p machine.
float SpeedOf(Machine &host, Machine &machine, ECS::Entity playerOnHost)
{
    static Runtime::AnimatorParam<float> speed{"speed"};
    ECS::Scene &scene = machine.world.scene;
    const ECS::Entity body = BodyOf(scene, CopyOn(host, machine, playerOnHost));
    REQUIRE(body != ECS::NullEntity);
    return speed.Get(*scene.Get<Runtime::AnimatorRun>(body)).value_or(-1.f);
}

/// The three players, as the host has them.
struct Players
{
    ECS::Entity host = ECS::NullEntity;
    ECS::Entity first = ECS::NullEntity;
    ECS::Entity second = ECS::NullEntity;
};

/// Drives @p machine's own player: @p move, and the buttons it holds.
void Drive(Machine &machine, glm::vec2 move, App::PlayerButtons held)
{
    const ECS::Entity own = LocalPlayerOf(machine.world.scene);
    if (App::PlayerInput *controls = machine.world.scene.GetMut<App::PlayerInput>(own))
    {
        controls->move = move;
        controls->held = held;
    }
}

/// Gives every machine in @p session the mesh and its players' params, as its
/// GPU cache and PlayerAnimation would, once a frame.
void Animate(Session &session, const Character &character)
{
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        BindMeshes(*machine, character);
        FeedPlayers(*machine);
    }
}

/// One frame of the host's player sprinting ahead and the first client's
/// crouching, the host and clients being @p session's first three machines.
void Act(Session &session, const Character &character)
{
    Drive(*session.machines[0], {0.f, 1.f}, App::PlayerButtons::Of(App::PlayerAction::Sprint));
    Drive(*session.machines[1], {0.f, 0.f}, App::PlayerButtons::Of(App::PlayerAction::Crouch));
    Animate(session, character);
}

/// How many times @p player has landed, as @p machine has it.
std::uint32_t LandingsOf(Machine &host, Machine &machine, ECS::Entity playerOnHost)
{
    return machine.world.scene.Get<Physics::CharacterState>(CopyOn(host, machine, playerOnHost))->landings;
}

/// Whether each machine has seen a player's body jump and land, and whether
/// one showed it landing before its character had.
struct JumpWatch
{
    std::array<std::uint32_t, 3> landingsBefore{};
    std::array<bool, 3> sawJump{};
    std::array<bool, 3> sawLand{};
    std::array<bool, 3> landedEarly{};

    /// Notes each machine's landings for @p playerOnHost before it jumps.
    void Start(Session &session, ECS::Entity playerOnHost)
    {
        for (std::size_t index = 0; index < session.machines.size(); ++index)
        {
            landingsBefore[index] = LandingsOf(*session.machines[0], *session.machines[index], playerOnHost);
        }
    }

    /// One frame of @p session, looking at the body of @p playerOnHost on each
    /// machine, the host being the first.
    void Frame(Session &session, const Character &character, ECS::Entity playerOnHost)
    {
        Animate(session, character);
        Machine &host = *session.machines[0];
        for (std::size_t index = 0; index < session.machines.size(); ++index)
        {
            Machine &machine = *session.machines[index];
            const std::vector<std::string> states = StatesOf(host, machine, playerOnHost);
            const bool landing = states == std::vector<std::string>{"land"};
            sawJump[index] = sawJump[index] || states == std::vector<std::string>{"jump"};
            sawLand[index] = sawLand[index] || landing;
            landedEarly[index] =
                landedEarly[index] || (landing && LandingsOf(host, machine, playerOnHost) == landingsBefore[index]);
        }
    }
};

/// A bell rung through a character, as the sample's: the asker names its
/// character, the host tells everyone who rang, and every machine plays that
/// player's gesture when it hears.
struct Gestures
{
    /// Asked on the next step, through this character, then cleared.
    ECS::Entity askThrough = ECS::NullEntity;

    explicit Gestures(App::World &world)
    {
        world.systems.Register(App::SystemPhase::FixedUpdate, "Ask", [this](App::SystemContext &ctx) { Ask(ctx); });
        world.systems.Register(App::SystemPhase::FixedUpdate, "Answer",
                               [](App::SystemContext &ctx) { Answer(ctx); });
        world.systems.Register(App::SystemPhase::FixedUpdate, "Hear", [](App::SystemContext &ctx) { Hear(ctx); });
    }

private:
    void Ask(App::SystemContext &ctx)
    {
        if (askThrough != ECS::NullEntity)
        {
            CHECK(ctx.net.SendIntent(App::Test::TestAskBell{.ringer = askThrough, .bell = 1}));
            askThrough = ECS::NullEntity;
        }
    }

    static void Answer(App::SystemContext &ctx)
    {
        for (const Core::Received<App::Test::TestAskBell> &received :
             ctx.events.Read<Core::Received<App::Test::TestAskBell>>())
        {
            CHECK(ctx.net.Send(App::Test::TestBellRang{.ringer = received.message.ringer,
                                                       .bell = received.message.bell}));
        }
    }

    static void Hear(App::SystemContext &ctx)
    {
        static Runtime::AnimatorTrigger interact{"interact"};
        for (const Core::Received<App::Test::TestBellRang> &received :
             ctx.events.Read<Core::Received<App::Test::TestBellRang>>())
        {
            const ECS::Entity body = BodyOf(ctx.world.scene, received.message.ringer);
            if (received.message.ringer != ECS::NullEntity && body != ECS::NullEntity)
            {
                interact.Fire(body, *ctx.world.scene.GetMut<Runtime::AnimatorRun>(body));
            }
        }
    }
};

/// Whether each machine has seen a player's body play its gesture.
struct GestureWatch
{
    std::array<bool, 3> saw{};

    void Frame(Session &session, const Character &character, ECS::Entity playerOnHost)
    {
        Animate(session, character);
        for (std::size_t index = 0; index < session.machines.size(); ++index)
        {
            const std::vector<std::string> states = StatesOf(*session.machines[0], *session.machines[index],
                                                             playerOnHost);
            saw[index] = saw[index] || states == std::vector<std::string>{"interact"};
        }
    }
};

/// The joint of the biped's body an item is held at, and where it sits from it (m).
constexpr std::string_view kHeldAt = "limb_l_end";
constexpr glm::vec3 kHeldOffset{0.f, 0.1f, 0.f};

/// Where a free item lies, picked up and dropped this many times (m).
constexpr glm::dvec3 kItemLies{1.0, 0.075, 1.0};
constexpr std::int32_t kHoldCycles = 5;

/// Frames an item is held, then lies free, in each cycle: long enough for
/// every machine to hear, and for what each draws to settle.
constexpr std::int32_t kHoldFrames = 40;

/// The item's size.
constexpr float kItemScale = 0.15f;

/// Hands @p item to @p handTo, or lays it free where it lies for NullEntity, once
/// it is set: its Transform the offset from the joint while held, where it lies
/// while free.
void HandItem(ECS::Scene &scene, ECS::Entity item, std::optional<ECS::Entity> &handTo)
{
    if (!handTo.has_value() || item == ECS::NullEntity)
    {
        return;
    }
    const bool held = *handTo != ECS::NullEntity;
    scene.GetMut<App::Test::TestHoldable>(item)->holder = *handTo;
    *scene.GetMut<ECS::Transform>(item) =
        ECS::Transform{.position = held ? glm::dvec3(kHeldOffset) : kItemLies, .scale = glm::vec3(kItemScale)};
    handTo.reset();
}

/// How far from where it should be a machine may draw an item (m).
constexpr float kHeldNear = 0.01f;

/// Hangs each held item from its holder's body at the joint, and lets go of a
/// free one, as the sample's HoldHoldables does on every machine.
void HoldItems(ECS::Scene &scene)
{
    std::vector<std::pair<ECS::Entity, ECS::Entity>> held;
    std::vector<ECS::Entity> loose;
    for (auto [item, holdable] : scene.Query<App::Test::TestHoldable>())
    {
        const ECS::Entity body =
            scene.IsAlive(holdable.holder) ? BodyOf(scene, holdable.holder) : ECS::NullEntity;
        if (body != ECS::NullEntity)
        {
            held.emplace_back(item, body);
        }
        else if (holdable.holder == ECS::NullEntity)
        {
            loose.push_back(item);
        }
    }
    for (const auto &[item, body] : held)
    {
        // The host's JointAttachment may have arrived already: it is sent.
        if (!scene.Has<ECS::Parent>(item))
        {
            REQUIRE(scene.Add(item, ECS::Parent{.parent = body}) != nullptr);
        }
        if (!scene.Has<Runtime::JointAttachment>(item))
        {
            REQUIRE(scene.Add(item, Runtime::JointAttachment{.joint = Core::InternedString{kHeldAt}}) != nullptr);
        }
    }
    for (const ECS::Entity item : loose)
    {
        (void)scene.Remove<Runtime::JointAttachment>(item);
        (void)scene.Remove<ECS::Parent>(item);
    }
}

/// How far from where it should be each machine draws the item, at its worst
/// over a cycle: at the holder's joint while that machine has it held, where
/// it lies once that machine has it free.
struct HeldWatch
{
    std::array<float, 3> worstHeld{};
    std::array<float, 3> worstFree{};

    void Frame(Session &session, const Character &character, ECS::Entity itemOnHost)
    {
        Animate(session, character);
        Machine &host = *session.machines[0];
        for (std::size_t index = 0; index < session.machines.size(); ++index)
        {
            Machine &machine = *session.machines[index];
            ECS::Scene &scene = machine.world.scene;
            const ECS::Entity item = CopyOn(host, machine, itemOnHost);
            if (item == ECS::NullEntity)
            {
                continue;
            }
            // Placed as the frame is drawn, after the view is smoothed.
            machine.world.propagationTick = ECS::PropagateTransforms(scene, machine.world.propagationTick);
            const glm::vec3 drawn = glm::vec3(ECS::WorldPositionOf(*scene.Get<ECS::WorldMatrix>(item)));
            const ECS::Entity holder = scene.Get<App::Test::TestHoldable>(item)->holder;
            if (holder == ECS::NullEntity)
            {
                worstFree[index] = std::max(worstFree[index], glm::length(drawn - glm::vec3(kItemLies)));
                continue;
            }
            const ECS::Entity body = BodyOf(scene, holder);
            const Runtime::SkinnedPose *pose = scene.Get<Runtime::SkinnedPose>(body);
            const int32_t joint = Geometry::FindJoint(character.mesh.Skeleton(), kHeldAt);
            if (pose == nullptr || pose->jointModel.size() <= static_cast<std::size_t>(joint))
            {
                continue;
            }
            const glm::mat4 bodyWorld = ECS::MatrixFrom(*scene.Get<ECS::WorldMatrix>(body), glm::dvec3(0.0));
            const glm::vec3 expected =
                glm::vec3(bodyWorld * pose->jointModel[static_cast<std::size_t>(joint)] * glm::vec4(kHeldOffset, 1.f));
            worstHeld[index] = std::max(worstHeld[index], glm::length(drawn - expected));
        }
    }
};

} // namespace

TEST_CASE("A held item is at its holder's joint on every machine from the frame it is held, however often it changes "
          "hands")
{
    const std::filesystem::path root = FreshRoot("assisi_players_bodies_held");
    WritePlayerWithBody(root);
    Character character;
    Build(character);

    const std::vector<std::string> animation{"Animators", "AnimationPlayers"};
    Session session;
    Machine &host = session.Add({}, animation);
    Machine &first = session.Add({}, animation);
    Machine &second = session.Add({}, animation);
    // What the host does with the item at its next Update, before the machines
    // hang it, as the sample's PickUpHoldables and DropHoldables do.
    std::optional<ECS::Entity> handTo;
    ECS::Entity item = ECS::NullEntity;
    host.world.systems.Register(App::SystemPhase::Update, "Hand",
                                [&](App::SystemContext &ctx) { HandItem(ctx.world.scene, item, handTo); });
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        Provide(*machine, character);
        machine->world.systems.Register(App::SystemPhase::Update, "Hold",
                                        [](App::SystemContext &ctx) { HoldItems(ctx.world.scene); });
    }
    AddFloors(session, glm::dvec3{0.0});
    JoinTwo(session, host, first, second, kStarts);
    const ECS::Entity holder = PlayerOf(host, second.play.Session().LocalClientId());
    REQUIRE(holder != ECS::NullEntity);

    item = host.world.scene.Create();
    REQUIRE(host.world.scene.Add(item, ECS::Transform{.position = kItemLies, .scale = glm::vec3(kItemScale)}) !=
            nullptr);
    REQUIRE(host.world.scene.Add(item, NetSync::Replicated{}) != nullptr);
    REQUIRE(host.world.scene.Add(item, App::Test::TestHoldable{}) != nullptr);
    session.Run(kActFrames, [&] { Animate(session, character); });

    for (int32_t cycle = 0; cycle < kHoldCycles; ++cycle)
    {
        CAPTURE(cycle);
        HeldWatch watch;
        handTo = holder;
        session.Run(kHoldFrames, [&] { watch.Frame(session, character, item); });
        handTo = ECS::NullEntity;
        session.Run(kHoldFrames, [&] { watch.Frame(session, character, item); });
        for (std::size_t index = 0; index < session.machines.size(); ++index)
        {
            CAPTURE(index);
            CHECK(watch.worstHeld[index] < kHeldNear);
            CHECK(watch.worstFree[index] < kHeldNear);
        }
    }
}

TEST_CASE("Every player's body animates on every machine as that player moves: sprinting, crouched, standing, "
          "jumping")
{
    const std::filesystem::path root = FreshRoot("assisi_players_bodies_animated");
    WritePlayerWithBody(root);
    Character character;
    Build(character);

    const std::vector<std::string> animation{"Animators", "AnimationPlayers"};
    Session session;
    Machine &host = session.Add({}, animation);
    Machine &first = session.Add({}, animation);
    Machine &second = session.Add({}, animation);
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        Provide(*machine, character);
    }
    AddFloors(session, glm::dvec3{0.0});
    JoinTwo(session, host, first, second, kStarts);

    const Players players{.host = PlayerOf(host, NetSync::HostClientId),
                          .first = PlayerOf(host, first.play.Session().LocalClientId()),
                          .second = PlayerOf(host, second.play.Session().LocalClientId())};
    REQUIRE(players.host != ECS::NullEntity);
    REQUIRE(players.first != ECS::NullEntity);
    REQUIRE(players.second != ECS::NullEntity);

    // The host's player sprints ahead, the first client's crouches, and the
    // second client's stands.
    session.Run(kActFrames, [&] { Act(session, character); });

    const float walkSpeed = host.world.scene.Get<Physics::CharacterLocomotion>(players.host)->walkSpeed;
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        CAPTURE(machine.get() == &host ? "host" : (machine.get() == &first ? "first" : "second"));
        CHECK(Joined(StatesOf(host, *machine, players.host)) == "locomotion");
        CHECK(SpeedOf(host, *machine, players.host) > walkSpeed * kSprintingAtLeast);
        CHECK(Joined(StatesOf(host, *machine, players.first)) == "crouch/crouch_idle");
        CHECK(Joined(StatesOf(host, *machine, players.second)) == "locomotion");
        CHECK(SpeedOf(host, *machine, players.second) < kStandingBelow);
    }

    // The second client's player jumps; every machine sees its body jump and
    // land, from the character state it was sent.
    const ECS::Entity jumper = LocalPlayerOf(second.world.scene);
    JumpWatch watch;
    watch.Start(session, players.second);
    second.world.scene.GetMut<App::PlayerInput>(jumper)->pressed = App::PlayerButtons::Of(App::PlayerAction::Jump);
    session.Run(kJumpFrames, [&] { watch.Frame(session, character, players.second); });
    for (std::size_t index = 0; index < session.machines.size(); ++index)
    {
        CAPTURE(index);
        CHECK(watch.sawJump[index]);
        CHECK(watch.sawLand[index]);
        // Landing follows the character's own landing, not the moment after
        // take-off when it still counts as on the ground.
        CHECK_FALSE(watch.landedEarly[index]);
    }
}

TEST_CASE("A gesture the host's message names plays on every machine, from the message")
{
    const std::filesystem::path root = FreshRoot("assisi_players_bodies_gesture");
    WritePlayerWithBody(root);
    Character character;
    Build(character);

    const std::vector<std::string> animation{"Animators", "AnimationPlayers"};
    Session session;
    Machine &host = session.Add({}, animation);
    Machine &first = session.Add({}, animation);
    Machine &second = session.Add({}, animation);
    std::vector<std::unique_ptr<Gestures>> gestures;
    for (const std::unique_ptr<Machine> &machine : session.machines)
    {
        Provide(*machine, character);
        gestures.push_back(std::make_unique<Gestures>(machine->world));
    }
    AddFloors(session, glm::dvec3{0.0});
    JoinTwo(session, host, first, second, kStarts);
    session.Run(kActFrames, [&] { Animate(session, character); });

    // The second client's player rings it, through its own character.
    const ECS::Entity ringer = PlayerOf(host, second.play.Session().LocalClientId());
    REQUIRE(ringer != ECS::NullEntity);
    gestures[2]->askThrough = LocalPlayerOf(second.world.scene);
    GestureWatch watch;
    session.Run(kActFrames, [&] { watch.Frame(session, character, ringer); });
    for (std::size_t index = 0; index < session.machines.size(); ++index)
    {
        CAPTURE(index);
        CHECK(watch.saw[index]);
    }
}
