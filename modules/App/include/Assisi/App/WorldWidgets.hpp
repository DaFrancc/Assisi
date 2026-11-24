/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file WorldWidgets.hpp
/// @brief UI that follows something in the world: a nameplate over a character,
/// a prompt on a door, a marker on an objective.
///
/// A world widget is an ordinary screen file, shown once for each entity that
/// asks for one, with that entity as its context so its bindings read it. The
/// one difference from any other screen is where it sits: at the entity's place
/// on screen, worked out each frame through the camera, rather than across the
/// window. The component is the request and holds no UI; the screens are made,
/// moved and taken away by WorldWidgetSystem, and are never saved or sent.
///
/// How a widget behaves when its entity is off screen, behind something or far
/// away differs from game to game and from entity to entity, so each is a
/// setting on the component rather than a choice made here.

#include <Assisi/App/World.hpp>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Runtime/Camera.hpp>

#include <cstdint>
#include <optional>

namespace Assisi::App
{

struct SystemContext;

/// @brief When a widget shows, by whether something stands between the camera
/// and its entity.
AENUM()
enum class WorldWidgetBlocked : uint8_t
{
    Ignored, ///< it shows whatever is in the way
    Hides,   ///< it shows only while nothing is in the way: a nameplate
    Shows,   ///< it shows only while something is: a marker seen through walls
    Count_
};

/// @brief What a widget does while its entity is off screen or behind the camera.
AENUM()
enum class WorldWidgetOffScreen : uint8_t
{
    Hides,      ///< it goes with the entity: off the edge, and hidden behind the camera
    StaysOnEdge, ///< it stays at the edge of the screen nearest its entity: a marker
    Count_
};

/// @brief Shows the screen @p screen over this entity, moved with it every frame.
///
/// Saved with the entity, as part of its level, and sent to every player with
/// it, so a character the host spawns wears its nameplate on every machine.
/// The screen it asks for is neither: each machine makes its own.
ACOMP(replicable)
struct WorldWidget
{
    /// The screen file shown, such as `ui/Nameplate.amdn`, with this entity as
    /// its context.
    AFIELD() Core::AssetId screen;
    /// Where on the entity the screen is placed, from the entity's position, in
    /// metres along the world's axes: `0 2 0` puts a nameplate two metres over
    /// a character's feet whichever way the character faces.
    AFIELD() glm::vec3 offset{0.f};
    /// The distance in metres from which the widget starts to fade.
    AFIELD() float fadeNear = 0.f;
    /// The distance in metres at which it has faded away; 0 never fades it.
    AFIELD() float fadeFar = 0.f;
    /// The distance in metres at which the widget is drawn at the size its file
    /// gives it. Farther, it shrinks as a thing that far away would; nearer, it
    /// stays that size. 0 never scales it.
    AFIELD() float scaleDistance = 0.f;
    /// Whether what stands between the camera and the widget hides it, shows
    /// it, or changes nothing.
    AFIELD() WorldWidgetBlocked blocked = WorldWidgetBlocked::Ignored;
    /// Whether it hides with its entity off screen, or stays on the edge.
    AFIELD() WorldWidgetOffScreen offScreen = WorldWidgetOffScreen::Hides;
};

/// @brief The screen WorldWidgetSystem made for this entity's WorldWidget, and
/// the file it was made from, so a widget given another file is made again.
/// Made and removed by that system alone; never saved. The screen is owned by
/// the entity (SetScreenOwner), so it goes with it however it goes.
ACOMP(transient)
struct WorldWidgetScreen
{
    ScreenId screen;
    Core::AssetId from;
};

/// @brief Where a point in the world lands on screen.
struct ProjectedPoint
{
    /// Device pixels from the top-left corner of the viewport. Behind the
    /// camera, the edge point in the point's direction.
    Mondrian::Point at;
    /// Whether the point is in front of the camera.
    bool inFront = false;
    /// Whether the point is inside the viewport.
    bool onScreen = false;
};

/// @brief Where @p point, in the world, lands in a @p viewport seen through
/// @p camera standing at @p cameraWorld: what a widget is placed by. A point
/// behind the camera lands on the viewport's edge in the direction it lies, so
/// a widget kept on the edge points the right way.
[[nodiscard]] ProjectedPoint ProjectToScreen(const glm::dvec3 &point, const ECS::WorldMatrix &cameraWorld,
                                             const Runtime::Camera &camera, Mondrian::Extent viewport);

/// @brief Shows, moves and takes away the screen of every WorldWidget in the
/// world, after the frame's poses and every other Ui system, so each sits where
/// its entity is drawn.
ASYSTEM(Ui, name = "WorldWidgets", last) void WorldWidgetSystem(SystemContext &ctx);

} // namespace Assisi::App
