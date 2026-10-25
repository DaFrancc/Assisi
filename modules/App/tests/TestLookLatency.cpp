/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestLookLatency.cpp
/// @brief The camera of this machine's player turns the frame the mouse does,
///        at every frame rate, while the fixed step still owns the real aim.
///
/// Frames are run as a game runs them: the frame's turning added to the
/// player's controls before its ticks, as PlayerGather does in PreUpdate, then
/// the ticks, the blend and the camera. How far the drawn camera trails the
/// turning so far is the look latency.

#include <doctest/doctest.h>

#include <Assisi/App/FixedStepClock.hpp>
#include <Assisi/App/PlayerComponents.hpp>
#include <Assisi/App/PlayerSystems.hpp>
#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/StepWorld.hpp>
#include <Assisi/App/SystemCatalog.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

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
constexpr uint32_t kMaxStepsPerFrame = 4;

/// How fast the player turns, in radians a second: well under half a turn over
/// a run, so a yaw read back never wraps.
constexpr double kTurnPerSecond = 1.0;

/// How long each frame rate is played for.
constexpr double kRunSeconds = 1.0;

/// How far the drawn camera may trail the turning, in seconds of turning: what
/// float rounding over a run leaves, far under a frame at any rate here.
constexpr double kNoLag = 1e-4;

/// Where the eye sits on the character.
constexpr float kEyeHeight = 1.5f;

/// A character played by this machine, with its eye, in a world ticked by hand.
struct Looker
{
    Core::EventQueue universal{Core::EventScope::Universal};
    App::WorldManager worlds{Physics::NoCollisionAssets()};
    App::World &world = worlds.Create("Look");
    ECS::Entity character = ECS::NullEntity;
    ECS::Entity eye = ECS::NullEntity;

    Looker()
    {
        REQUIRE(App::SystemCatalog::Instance().Install(world, std::vector<std::string>{"CharacterCommand"}, "test"));
        ECS::Scene &scene = world.scene;
        character = scene.Create();
        (void)scene.Add<ECS::Transform>(character);
        (void)scene.Add<App::Player>(character);
        (void)scene.Add<App::LocalPlayer>(character);
        (void)scene.Add<App::PlayerInput>(character);
        (void)scene.Add<Physics::CharacterIntent>(character);

        eye = scene.Create();
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

    /// The frame's mouse movement, added as PlayerGather adds it.
    void Turn(float yaw, float pitch)
    {
        App::PlayerInput &controls = *world.scene.GetMut<App::PlayerInput>(character);
        controls.lookYaw += yaw;
        controls.lookPitch += pitch;
    }

    /// The frame's ticks, then the pose the frame is drawn from.
    glm::mat4 Frame(App::FixedStepClock &clock, double seconds)
    {
        const uint32_t steps = clock.Advance(seconds);
        for (uint32_t step = 0; step < steps; ++step)
        {
            App::StepWorld(Context());
        }
        App::BlendWorld(world, /*stepping=*/ true, static_cast<float>(clock.Alpha()));
        world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
        const std::optional<App::SceneView> view = App::ActiveSceneCamera(world.scene);
        REQUIRE(view.has_value());
        return view->world;
    }
};

/// How far left of straight ahead @p world looks, in radians.
double YawOf(const glm::mat4 &world)
{
    const glm::vec3 forward = glm::mat3(world) * glm::vec3(0.f, 0.f, -1.f);
    return std::atan2(-static_cast<double>(forward.x), -static_cast<double>(forward.z));
}

/// How far up @p world looks, in radians.
double PitchOf(const glm::mat4 &world)
{
    const glm::vec3 forward = glm::normalize(glm::mat3(world) * glm::vec3(0.f, 0.f, -1.f));
    return std::asin(static_cast<double>(glm::clamp(forward.y, -1.f, 1.f)));
}

/// Plays a turn at @p fps and returns the most the drawn camera trailed it, in
/// seconds of turning.
double WorstLookLag(double fps)
{
    Looker looker;
    App::FixedStepClock clock(kStep, kMaxStepsPerFrame);
    const double frameSeconds = 1.0 / fps;
    const float turnPerFrame = static_cast<float>(kTurnPerSecond * frameSeconds);
    const int32_t frames = static_cast<int32_t>(std::lround(kRunSeconds * fps));

    double turned = 0.0;
    double worst = 0.0;
    for (int32_t frame = 0; frame < frames; ++frame)
    {
        looker.Turn(turnPerFrame, 0.f);
        turned += static_cast<double>(turnPerFrame);
        const double drawn = YawOf(looker.Frame(clock, frameSeconds));
        worst = std::max(worst, (turned - drawn) / kTurnPerSecond);
    }
    return worst;
}

} // namespace

TEST_CASE("The player's camera turns the frame the mouse does, at every frame rate")
{
    for (const double fps : {30.0, 45.0, 60.0, 90.0, 144.0, 240.0})
    {
        CAPTURE(fps);
        const double lag = WorstLookLag(fps);
        MESSAGE("look lag at ", fps, " fps: ", lag * 1000.0, " ms");
        CHECK(lag < kNoLag);
    }
}

TEST_CASE("The player's camera tilts the frame the mouse does, and no further than it may")
{
    Looker looker;
    App::FixedStepClock clock(kStep, kMaxStepsPerFrame);
    const double halfStep = static_cast<double>(kStep) / 2.0;

    constexpr float kTilt = 0.3f;
    looker.Turn(0.f, kTilt);
    CHECK(PitchOf(looker.Frame(clock, halfStep)) == doctest::Approx(kTilt));

    // Far past straight up: drawn at the limit, as the step will set it.
    looker.Turn(0.f, glm::radians(180.f));
    CHECK(PitchOf(looker.Frame(clock, halfStep)) == doctest::Approx(glm::radians(App::kMaxPitchDegrees)));
}

TEST_CASE("A camera that is not this machine's player's is drawn where the blend puts it")
{
    Looker looker;
    ECS::Scene &scene = looker.world.scene;
    (void)scene.Remove<App::LocalPlayer>(looker.character);
    App::FixedStepClock clock(kStep, kMaxStepsPerFrame);

    looker.Turn(0.5f, 0.f);
    const glm::mat4 drawn = looker.Frame(clock, static_cast<double>(kStep) / 2.0);
    CHECK(YawOf(drawn) == doctest::Approx(YawOf(scene.Get<ECS::WorldMatrix>(looker.eye)->matrix)));
    CHECK(YawOf(drawn) == doctest::Approx(0.0));
}
