/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "RandomLaunch.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <cmath>
#include <cstddef>
#include <random>
#include <vector>

namespace Game
{

namespace
{

/// A unit direction drawn evenly from the half of the sphere above the
/// horizontal.
glm::vec3 UpwardDirection(std::mt19937 &random)
{
    // On a sphere the height is spread evenly, so drawing it straight and the
    // angle around separately gives every direction the same chance.
    std::uniform_real_distribution<float> height(0.f, 1.f);
    std::uniform_real_distribution<float> around(0.f, glm::two_pi<float>());
    const float up = height(random);
    const float angle = around(random);
    const float across = std::sqrt(1.f - up * up);
    return glm::vec3(across * std::cos(angle), up, across * std::sin(angle));
}

} // namespace

void RandomLaunchSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.input == nullptr || !ctx.input->IsKeyPressed(Assisi::Window::Key::H))
    {
        return;
    }

    std::vector<Assisi::ECS::Entity> launchers;
    for (auto [entity, launch] : ctx.world.scene.Query<RandomLaunch>())
    {
        (void)launch;
        launchers.push_back(entity);
    }
    if (launchers.empty())
    {
        return;
    }

    // Seeded afresh per press: a press is rare, and a generator kept between
    // presses would be state a system installed in several worlds must not hold.
    std::mt19937 random{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick(0, launchers.size() - 1);
    const Assisi::ECS::Entity chosen = launchers[pick(random)];
    const float impulse = ctx.world.scene.Get<RandomLaunch>(chosen)->impulse;
    ctx.world.physics.AddImpulse(chosen, UpwardDirection(random) * impulse);
}

} // namespace Game
