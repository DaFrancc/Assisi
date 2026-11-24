/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenSurfaces.hpp
/// @brief A UI screen shown on a mesh in the world, such as a computer monitor
/// or a cockpit display, and worked from inside the game.
///
/// ScreenSurface is only a note on an entity saying "show this screen on me".
/// The ScreenSurfaces system draws the screen into a picture of its own and has
/// one material slot of the entity's mesh show it, as its base colour and its
/// glow. A picture is drawn again only when its screen changes, so a room of
/// idle monitors costs no UI work.
///
/// The player works one by having the game call UseScreenSurface, say when they
/// press a key looking at it: the cursor is freed, the player's own controls
/// are let go, and the pointer is cast from the camera onto the mesh, where it
/// lands on the screen. Back, or LeaveScreenSurface, hands everything back.

#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/World.hpp>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Mondrian/DrawList.hpp>
#include <Assisi/Mondrian/Target.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Assisi::App
{

struct SystemContext;

/// @brief A surface's picture size when nothing says otherwise, in pixels a
/// side.
inline constexpr uint32_t kDefaultSurfaceSide = 512;

/// @brief Shows a screen file on one material slot of this entity's mesh.
///
/// The screen's context is this entity, so its bindings read it: a monitor
/// on a turret shows that turret's ammunition. Its picture is @c width by
/// @c height pixels, and the screen is laid out against that size and scaled by
/// it alone, so a sharper picture shows the same layout more finely. The slot
/// keeps the rest of its material: give it an emissive factor for the screen to
/// glow in the dark, or leave it at zero for one only lit by the room.
///
/// Saved with its level and replicable, so a monitor the host spawns shows its
/// screen on every machine. Each machine draws its own: what a screen shows is
/// never sent.
ACOMP(replicable)
struct ScreenSurface
{
    /// The screen file to show.
    AFIELD() Core::AssetId screen;
    /// The picture's size, in pixels.
    AFIELD() uint32_t width = kDefaultSurfaceSide;
    AFIELD() uint32_t height = kDefaultSurfaceSide;
    /// Which material slot of the mesh shows it. A primitive mesh has one, 0.
    AFIELD() uint32_t materialSlot = 0;
};

/// @brief One triangle of a surface, in its mesh's own space, with the texture
/// coordinates at its corners.
struct SurfaceTriangle
{
    std::array<glm::vec3, 3> corners;
    std::array<glm::vec2, 3> uvs;
};

/// @brief The triangles a surface's slot draws, which a pointer is cast onto.
struct SurfaceGeometry
{
    std::vector<SurfaceTriangle> triangles;
};

/// @brief What the ScreenSurfaces system made for an entity's ScreenSurface.
/// Never saved or sent: every machine makes its own.
ACOMP(transient)
struct ScreenSurfaceScreen
{
    /// The slot's triangles, for casting the pointer; null when the mesh
    /// could not be read.
    std::shared_ptr<const SurfaceGeometry> geometry;
    ScreenId screen;
    /// What it was made from, so a change to any of it makes it again.
    Core::AssetId from;
    Core::AssetId mesh;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t materialSlot = 0;
};

/// @brief The triangles of @p mesh's material slot @p slot at its finest level,
/// with their texture coordinates. A mesh with no submeshes is one slot, 0.
[[nodiscard]] SurfaceGeometry SurfaceOf(const Geometry::MeshData &mesh, uint32_t slot);

/// @brief Where the ray from @p origin along @p direction, both in the
/// surface's own space, first meets @p surface ahead of @p origin, as texture
/// coordinates. Either side of a triangle counts. Nullopt where it misses.
[[nodiscard]] std::optional<glm::vec2> SurfaceHit(const SurfaceGeometry &surface, const glm::vec3 &origin,
                                                  const glm::vec3 &direction);

/// @brief What a surface's pointer is cast through: the camera and the surface
/// as they were the frame it was made, which is the frame the player saw.
struct SurfaceSight
{
    std::shared_ptr<const SurfaceGeometry> geometry;
    /// From the camera's space to the surface's own.
    glm::mat4 cameraToSurface{1.f};
    /// From clip space back to the camera's.
    glm::mat4 inverseProjection{1.f};
    Mondrian::Extent viewport;
    Mondrian::Extent picture;
};

/// @brief How a pointer in a @p viewport window, seen through @p view, is cast
/// onto @p geometry placed at @p surfaceWorld, landing in a @p picture.
[[nodiscard]] SurfaceSight SightOf(std::shared_ptr<const SurfaceGeometry> geometry,
                                   const ECS::WorldMatrix &surfaceWorld, const SceneView &view,
                                   Mondrian::Extent viewport, Mondrian::Extent picture);

/// @brief Where the window pointer at @p pointer, in device pixels, lands on
/// @p sight's picture, in its pixels; nullopt where it misses the surface.
[[nodiscard]] std::optional<Mondrian::Point> PointOnSurface(const SurfaceSight &sight, Mondrian::Point pointer);

/// @brief Hands the keys and the pointer to the screen on @p entity's
/// ScreenSurface, freeing the cursor and letting go of the player's own
/// controls until Back or LeaveScreenSurface. False when it has no screen yet
/// or the host shows no UI.
bool UseScreenSurface(SystemContext &ctx, ECS::Entity entity);

/// @brief Hands the keys and the pointer back from whichever surface is used.
void LeaveScreenSurface(SystemContext &ctx);

/// @brief Shows each ScreenSurface's screen on its mesh, makes it again when
/// the component changes and takes it away when the component goes, and casts
/// the pointer onto the surface being used.
///
/// @par Requirements
/// A level or blueprint runs it, in the Ui phase, last, so a camera an earlier
/// Ui system moved is where the pointer is cast from. A headless host shows
/// nothing.
ASYSTEM(Ui, name = "ScreenSurfaces", last) void ScreenSurfaceSystem(SystemContext &ctx);

} // namespace Assisi::App
