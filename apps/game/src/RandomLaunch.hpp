/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file RandomLaunch.hpp
/// @brief Pressing H kicks one body, picked at random, in a random direction.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// Marks a body that H may launch, and how hard.
///
/// The kick is an impulse, so a heavier body flies slower for the same
/// `impulse`: a body of mass m leaves at impulse / m metres per second.
ACOMP()
struct RandomLaunch
{
    /// N·s.
    AFIELD(min = 0.0) float impulse = 50000.f;
};

/// On H, picks one entity with a RandomLaunch and kicks it by its `impulse`
/// in a random direction above the horizontal: one into the ground would only
/// press it into whatever it rests on.
///
/// Update, because a key press is seen on one frame and FixedUpdate may run
/// that frame several times or not at all. The kick waits for the next physics
/// step like any other push. `activeWorldOnly`, and it null-checks input: a
/// headless host has no devices.
ASYSTEM(Update, name = "RandomLaunch", activeWorldOnly)
void RandomLaunchSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
