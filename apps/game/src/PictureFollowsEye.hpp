/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file PictureFollowsEye.hpp
/// @brief Moving a camera picture's darkening with the player's eye, as a
///        rifle scope's does: the book's example of Runtime::PictureVignette.
///
/// In levels/Test.alvl the security screen's picture darkens toward its edge,
/// and the clear part slides away from the side the player stands on as they
/// walk around the screen.

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

namespace Game
{

/// Sets the PictureVignette centre of every camera a CameraSurface shows from
/// how far off the surface's axis the active camera is. A camera shown on more
/// than one surface follows the last one found. `activeWorldOnly`, since only
/// the world being looked at has an eye to follow.
ASYSTEM(Update, name = "PictureFollowsEye", activeWorldOnly)
void PictureFollowsEyeSystem(Assisi::App::SystemContext &ctx);

} // namespace Game
