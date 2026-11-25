/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestFirstPersonLook.cpp
/// @brief The first-person template's look: the character turns, its view
///        camera tilts within its limit, and this machine's player's view
///        follows the mouse every frame while the fixed step keeps the real aim.
///
/// The controls are written directly, as PlayerGather would fill them in.
/// Frames are run as a game runs them: the frame's turning added before its
/// ticks, then the ticks, the blend, the template's LookAhead and the camera.

#include "Player/CameraRig.hpp"
#include "Player/Look.hpp"

#include <Assisi/App/FixedStepClock.hpp>
#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSystems.hpp>
#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/DrawPose.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Assisi;

namespace
{

constexpr float kStep = 1.f / 60.f;

/// The most fixed steps a frame runs, as the hosts cap them.
constexpr std::uint32_t kMaxStepsPerFrame = 4;

/// Close enough for an angle that went through a quaternion.
constexpr double kTolerance = 1e-4;

/// How fast the player turns, in radians a second: well under half a turn over
/// a run, so a yaw read back never wraps.
constexpr double kTurnPerSecond = 1.0;

/// How long each frame rate is played for.
constexpr double kRunSeconds = 1.0;

/// How far the drawn view may trail the turning, in seconds of turning: what
/// float rounding over a run leaves, far under a frame at any rate here.
constexpr double kNoLag = 1e-4;

/// Where the view camera sits on the character.
constexpr float kEyeHeight = 1.5f;

/// A character played by this machine with the template's look, in a world
/// ticked by hand.
struct Looker
{
    Core::EventQueue universal{Core::EventScope::Universal};
    App::WorldManager worlds{Physics::NoCollisionAssets()};
    App::World &world = worlds.Create("Look");
    ECS::Entity character = ECS::NullEntity;
    ECS::Entity eye = ECS::NullEntity;

    Looker()
    {
        REQUIRE(App::SystemCatalog::Instance().Install(world, std::vector<std::string>{"Look"}, "test"));
        ECS::Scene &scene = world.scene;
        character = scene.Create();
        eye = scene.Create();
        (void)scene.Add<ECS::Transform>(character);
        (void)scene.Add<App::Player>(character);
        (void)scene.Add<App::LocalPlayer>(character);
        (void)scene.Add<App::PlayerInput>(character);
        (void)scene.Add<Game::Look>(character);
        (void)scene.Add<Game::ViewRig>(character, Game::ViewRig{.camera = eye});

        (void)scene.Add<ECS::Transform>(eye, ECS::Transform{.position = {0.f, kEyeHeight, 0.f}});
        (void)scene.Add<Runtime::Camera>(eye, Runtime::Camera{.isActive = true});
        (void)scene.Add<ECS::Parent>(eye, ECS::Parent{.parent = character});
        world.propagationTick = ECS::PropagateTransforms(scene, world.propagationTick);
    }

    App::SystemContext Context()
    {
        return {.world = world,
                .dt = kStep,
                .simTick = 0,
                .input = nullptr,
                .actions = nullptr,
                .universalEvents = universal,
                .isActiveWorld = true,
                .worldManager = &worlds};
    }

    App::PlayerInput &Controls() { return *world.scene.GetMut<App::PlayerInput>(character); }

    /// The frame's mouse movement, added as PlayerGather adds it.
    void Turn(float yaw, float pitch)
    {
        Controls().lookYaw += yaw;
        Controls().lookPitch += pitch;
    }

    /// The frame's ticks, the template's LookAhead, then the pose the frame is
    /// drawn from.
    glm::mat4 Frame(App::FixedStepClock &clock, double seconds)
    {
        const std::uint32_t steps = clock.Advance(seconds);
        for (std::uint32_t step = 0; step < steps; ++step)
        {
            App::StepWorld(Context());
        }
        App::BlendWorld(world, /*stepping=*/ true, static_cast<float>(clock.Alpha()));
        world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
        App::SystemContext frame = Context();
        Game::LookAheadSystem(frame);
        // As the game draws: its update systems, then the transform pass, which
        // applies the view's draw pose.
        world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
        const std::optional<App::SceneView> view = App::ActiveSceneCamera(world.scene);
        REQUIRE(view.has_value());
        return view->world.inCell;
    }
};

/// How far left of straight ahead @p rotation looks, in radians.
double YawOf(const glm::quat &rotation)
{
    const glm::vec3 forward = rotation * glm::vec3(0.f, 0.f, -1.f);
    return std::atan2(-static_cast<double>(forward.x), -static_cast<double>(forward.z));
}

double YawOf(const glm::mat4 &world)
{
    return YawOf(glm::quat_cast(glm::mat3(world)));
}

/// How far up @p rotation looks, in radians.
double PitchOf(const glm::quat &rotation)
{
    const glm::vec3 forward = glm::normalize(rotation * glm::vec3(0.f, 0.f, -1.f));
    return std::asin(static_cast<double>(glm::clamp(forward.y, -1.f, 1.f)));
}

double PitchOf(const glm::mat4 &world)
{
    return PitchOf(glm::quat_cast(glm::mat3(world)));
}

/// Plays a turn at @p fps and returns the most the drawn view trailed it, in
/// seconds of turning.
double WorstLookLag(double fps)
{
    Looker looker;
    App::FixedStepClock clock(kStep, kMaxStepsPerFrame);
    const double frameSeconds = 1.0 / fps;
    const float turnPerFrame = static_cast<float>(kTurnPerSecond * frameSeconds);
    const std::int32_t frames = static_cast<std::int32_t>(std::lround(kRunSeconds * fps));

    double turned = 0.0;
    double worst = 0.0;
    for (std::int32_t frame = 0; frame < frames; ++frame)
    {
        looker.Turn(turnPerFrame, 0.f);
        turned += static_cast<double>(turnPerFrame);
        const double drawn = YawOf(looker.Frame(clock, frameSeconds));
        worst = std::max(worst, (turned - drawn) / kTurnPerSecond);
    }
    return worst;
}

} // namespace

TEST_CASE("Look turns the character about world up and tilts its view camera, once")
{
    Looker looker;
    App::SystemContext ctx = looker.Context();
    const float turn = glm::radians(30.f);
    const float lookUp = glm::radians(20.f);
    looker.Turn(turn, lookUp);

    App::StepWorld(ctx);
    const ECS::Scene &scene = looker.world.scene;
    CHECK(YawOf(scene.Get<ECS::Transform>(looker.character)->rotation) == doctest::Approx(turn).epsilon(kTolerance));
    CHECK(PitchOf(scene.Get<ECS::Transform>(looker.character)->rotation) == doctest::Approx(0.0).epsilon(kTolerance));
    CHECK(PitchOf(scene.Get<ECS::Transform>(looker.eye)->rotation) == doctest::Approx(lookUp).epsilon(kTolerance));

    // A second step with nothing new turns nothing: the engine cleared the
    // turning the first one used.
    App::StepWorld(ctx);
    CHECK(YawOf(scene.Get<ECS::Transform>(looker.character)->rotation) == doctest::Approx(turn).epsilon(kTolerance));
}

TEST_CASE("Look tilts no further than the character's Look allows")
{
    Looker looker;
    App::SystemContext ctx = looker.Context();

    looker.Turn(0.f, glm::radians(180.f));
    App::StepWorld(ctx);
    CHECK(PitchOf(looker.world.scene.Get<ECS::Transform>(looker.eye)->rotation) ==
          doctest::Approx(glm::radians(Game::kMaxLookPitchDegrees)).epsilon(kTolerance));

    looker.world.scene.GetMut<Game::Look>(looker.character)->maxPitchDegrees = 30.f;
    looker.Turn(0.f, -glm::radians(180.f));
    App::StepWorld(ctx);
    CHECK(PitchOf(looker.world.scene.Get<ECS::Transform>(looker.eye)->rotation) ==
          doctest::Approx(-glm::radians(30.0)).epsilon(kTolerance));
}

TEST_CASE("A replayed step turns the character again from its controls, as the live step did")
{
    // Look is predicted: a client stepping a tick again turns from the controls
    // it recorded, so a rollback puts the character back where it was facing.
    Looker looker;
    App::SystemContext ctx = looker.Context();
    ctx.replaying = true;
    looker.Turn(glm::radians(15.f), 0.f);

    App::StepWorld(ctx);
    CHECK(YawOf(looker.world.scene.Get<ECS::Transform>(looker.character)->rotation) ==
          doctest::Approx(glm::radians(15.0)).epsilon(kTolerance));
}

TEST_CASE("The player's view turns the frame the mouse does, at every frame rate")
{
    for (const double fps : {30.0, 45.0, 60.0, 90.0, 144.0, 240.0})
    {
        CAPTURE(fps);
        const double lag = WorstLookLag(fps);
        MESSAGE("look lag at ", fps, " fps: ", lag * 1000.0, " ms");
        CHECK(lag < kNoLag);
    }
}

TEST_CASE("The player's view tilts the frame the mouse does, and no further than the step will")
{
    Looker looker;
    App::FixedStepClock clock(kStep, kMaxStepsPerFrame);
    const double halfStep = static_cast<double>(kStep) / 2.0;

    constexpr float kTilt = 0.3f;
    looker.Turn(0.f, kTilt);
    CHECK(PitchOf(looker.Frame(clock, halfStep)) == doctest::Approx(kTilt));

    // Far past straight up: drawn at the limit, as the step will set it.
    looker.Turn(0.f, glm::radians(180.f));
    CHECK(PitchOf(looker.Frame(clock, halfStep)) == doctest::Approx(glm::radians(Game::kMaxLookPitchDegrees)));
}

TEST_CASE("Another player's view camera is drawn where the blend puts it, with no pose left from before")
{
    Looker looker;
    ECS::Scene &scene = looker.world.scene;
    App::FixedStepClock clock(kStep, kMaxStepsPerFrame);
    const double halfStep = static_cast<double>(kStep) / 2.0;

    // This machine's player first, so its camera has a pose to leave behind.
    looker.Turn(0.5f, 0.f);
    (void)looker.Frame(clock, halfStep);
    REQUIRE(scene.Get<ECS::DrawPose>(looker.eye) != nullptr);

    (void)scene.Remove<App::LocalPlayer>(looker.character);
    const glm::mat4 drawn = looker.Frame(clock, halfStep);
    CHECK(scene.Get<ECS::DrawPose>(looker.eye) == nullptr);
    CHECK(YawOf(drawn) == doctest::Approx(YawOf(scene.Get<ECS::WorldMatrix>(looker.eye)->inCell)));
}
