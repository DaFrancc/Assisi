/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TextureCamera.hpp
/// @brief A camera that draws into a picture a mesh in the world shows, such as
/// a rifle scope's glass or a security monitor.
///
/// TextureCamera goes on an entity with a Camera and a Transform; CameraSurface
/// goes on a mesh and names the camera whose picture it shows. The renderer
/// draws each texture camera that is needed this frame before the main camera,
/// and the surface shows its picture as light at the brightness the camera saw,
/// so the main camera's exposure covers it with everything else.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/Frustum.hpp>
#include <Assisi/Render/PictureLevels.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Assisi::Runtime
{

/// @brief A texture camera's picture size when nothing says otherwise, in
/// pixels a side.
inline constexpr uint32_t kDefaultTextureCameraSide = 512;

/// @brief A texture camera's MSAA samples a pixel when nothing says otherwise:
/// smooth edges on a screen seen up close, at a small cost for a small picture.
inline constexpr uint32_t kDefaultTextureCameraSamples = 4;

/// @brief When a TextureCamera draws its picture. A camera that doesn't draw
/// on a frame leaves its last picture on its surfaces.
AENUM()
enum class TextureCameraUpdate : uint8_t
{
    Always,    ///< Every frame.
    WhileSeen, ///< On a frame when a surface showing it is in view of a camera that draws.
    WhenAsked, ///< Once, the next time the scene is drawn after RequestTextureCameraPicture.
    WhileOn,   ///< Every frame while switched on with SwitchTextureCamera.
    Count_,
};

/// @brief Which way SwitchTextureCamera turns a camera.
enum class TextureCameraSwitch : uint8_t
{
    Stop,  ///< Stops drawing; its surfaces keep the last picture.
    Start, ///< Draws every frame until stopped.
    Count_,
};

/// @brief Draws what its entity's Camera sees into a picture, instead of to
/// the screen, for the meshes with a CameraSurface naming it to show.
///
/// The Camera's lens applies, its aspect set by the picture's size. A texture
/// camera is never the one the player looks through, whatever its Camera's
/// `isActive` says. It shares the main camera's sun shadows, so nothing outside
/// what the main camera's shadows cover has a sun shadow in its picture.
///
/// Saved with its level. Not sent: each machine draws its own picture.
ACOMP(requires = {Camera})
struct TextureCamera
{
    /// The picture's size, in pixels.
    AFIELD(min = 1) uint32_t width = kDefaultTextureCameraSide;
    AFIELD(min = 1) uint32_t height = kDefaultTextureCameraSide;
    /// MSAA samples a pixel, whatever the game's own anti-aliasing: 1 for
    /// none, up to 8. A count that isn't a power of two takes the one below,
    /// and one past what the GPU can do takes the most it can.
    AFIELD(min = 1, max = 8) uint32_t samples = kDefaultTextureCameraSamples;
    /// When it draws.
    AFIELD() TextureCameraUpdate update = TextureCameraUpdate::WhileSeen;
    /// Whether a WhileOn camera is drawing, which SwitchTextureCamera sets. A
    /// level can place one switched off.
    AFIELD() bool on = true;
    /// Whether it draws at about as many pixels as its surface covers on the
    /// main camera's screen, up to its own size: a distant monitor draws few.
    /// A camera whose surfaces are all off screen draws at its full size.
    AFIELD() bool followScreenSize = true;
};

/// @brief How far a PictureVignette's picture stays clear from its centre when
/// nothing says otherwise, in picture heights: with the default softness the
/// dark band just reaches a square picture's edges.
inline constexpr float kDefaultVignetteRadius = 0.4f;

/// @brief How wide a PictureVignette's fade is when nothing says otherwise, in
/// picture heights.
inline constexpr float kDefaultVignetteSoftness = 0.1f;

/// @brief Darkens a TextureCamera's picture toward its edge, as a rifle
/// scope's view darkens when the eye is off the scope's axis.
///
/// The picture stays clear inside a circle around `centre`, fades over
/// `softness` past `radius`, and beyond is darkened by `strength`. Distances
/// are in picture heights, so the circle stays round on a wide picture. A game
/// sets it from a system each frame, such as moving the centre opposite to
/// where the eye is, and the surfaces show the change that same frame, even
/// when the camera doesn't draw: only the picture's darkening is redone then.
///
/// Saved with its level. Not sent: each machine darkens its own picture.
ACOMP(requires = {TextureCamera})
struct PictureVignette
{
    /// The clear circle's middle, in the picture's own coordinates: 0 to 1
    /// across and down from its top-left corner.
    AFIELD() glm::vec2 centre{0.5f, 0.5f};
    /// How far from the centre the picture stays clear.
    AFIELD(min = 0) float radius = kDefaultVignetteRadius;
    /// How wide the fade from clear to dark is, past the radius.
    AFIELD(min = 0) float softness = kDefaultVignetteSoftness;
    /// How dark it gets beyond the fade: 0 for no darkening, 1 for black.
    AFIELD(min = 0, max = 1) float strength = 1.f;
};

/// @brief The darkening @p camera's picture is drawn with: none without a
/// PictureVignette, and its fields kept within their ranges with one.
[[nodiscard]] Render::Vignette PictureVignetteOf(const ECS::Scene &scene, ECS::Entity camera);

/// @brief Whether a picture's darkening is redone this frame without the
/// camera drawing: when the camera doesn't draw (@p drawing false), it
/// @p holdsDrawing to darken, and the darkening it wants, @p wanted, isn't
/// the one its picture has, @p shown. A camera that draws darkens its new
/// picture anyway.
[[nodiscard]] bool ShadePictureAgain(bool drawing, bool holdsDrawing, const Render::Vignette &wanted,
                                     const Render::Vignette &shown);

/// @brief How many steps a texture camera's drawn size is chosen in: its full
/// size, or a whole number of these fractions of it.
inline constexpr uint32_t kPictureDrawSteps = 8;

/// @brief How many kPictureDrawSteps of its size a picture whose longer side
/// is @p pictureSide draws at, when its surface covers @p surfacePixels on
/// screen and it drew at @p current steps last time (0 before it first
/// drew). None on screen, a @p surfacePixels of 0, draws at full size. It
/// grows at once and shrinks only by more than one step, so a surface on the
/// edge between two sizes doesn't switch between them every frame.
[[nodiscard]] uint32_t PictureDrawSteps(float surfacePixels, uint32_t pictureSide, uint32_t current);

/// @brief The side @p steps of kPictureDrawSteps make of @p side: at least
/// one pixel, rounded up.
[[nodiscard]] uint32_t PictureDrawnSide(uint32_t side, uint32_t steps);

/// @brief The MSAA samples a texture camera asking @p asked draws with on a
/// GPU that does at most @p most: a power of two from 1 to 8, the one at or
/// below what it asked, and no more than the GPU does.
[[nodiscard]] uint32_t TextureCameraSamples(uint32_t asked, uint32_t most);

/// @brief Asks a WhenAsked TextureCamera for one picture. Put on by
/// RequestTextureCameraPicture and taken off by the renderer when it draws.
ACOMP(transient)
struct TextureCameraRequest
{
};

/// @brief Shows a TextureCamera's picture on one material slot of this
/// entity's mesh.
///
/// The slot shows the picture as light, at the brightness the camera saw, so a
/// scope's glass matches the world around it; the rest of the slot's material
/// is not drawn. The camera never draws this surface itself, so a camera that
/// can see its own screen doesn't show the screen inside its picture.
///
/// Saved with its level. Not sent, since the camera it names is an entity of
/// this machine's own: put it on the mesh in the level or blueprint, where
/// every machine has it.
ACOMP(requires = {MeshRenderer})
struct CameraSurface
{
    /// The entity whose TextureCamera's picture it shows.
    AFIELD() ECS::Entity camera = ECS::NullEntity;
    /// Which material slot of the mesh shows it. A primitive mesh has one, 0.
    AFIELD() uint32_t materialSlot = 0;
};

/// @brief Starts or stops a WhileOn TextureCamera on @p camera. Starting a
/// camera that is on, or stopping one that is off, changes nothing. Nothing
/// happens to an entity without a TextureCamera.
void SwitchTextureCamera(ECS::Scene &scene, ECS::Entity camera, TextureCameraSwitch to);

/// @brief Asks the WhenAsked TextureCamera on @p camera for one picture, drawn
/// the next time the scene is drawn: this frame when asked from an update.
/// Asking again before it is drawn asks for the same one.
void RequestTextureCameraPicture(ECS::Scene &scene, ECS::Entity camera);

/// @brief A texture camera as the choice of which ones draw sees it.
struct TextureCameraCandidate
{
    /// What the camera sees, measured from the same origin as every other
    /// frustum and bound handed to the choice.
    Render::Frustum frustum;
    ECS::Entity camera = ECS::NullEntity;
    TextureCameraUpdate update = TextureCameraUpdate::WhileSeen;
    bool on = true;
    /// Whether a picture was asked for.
    bool asked = false;
    /// Whether it draws this frame whatever its mode says, as when its picture
    /// was made again at a new size and holds nothing yet.
    bool redraw = false;
};

/// @brief A surface showing a camera's picture, as the choice sees it.
struct CameraSurfaceBounds
{
    /// The surface's bounds, measured from the same origin as the frusta.
    Geometry::BoundingSphere bounds;
    /// The camera whose picture it shows.
    ECS::Entity camera = ECS::NullEntity;
};

/// @brief Which texture cameras draw this frame, written to @p order in the
/// order they draw.
///
/// A camera draws when its mode says so or it must redraw, or, WhileSeen, when a surface showing
/// it is in the frustum of the main camera (@p main) or of another camera that
/// draws. A camera never counts as seeing its own surfaces. Each camera comes
/// after every camera whose surface it sees, so the picture it shows is this
/// frame's; where two see each other, one of them shows the other's last
/// picture.
void OrderTextureCameras(const Render::Frustum &main, std::span<const TextureCameraCandidate> cameras,
                         std::span<const CameraSurfaceBounds> surfaces, std::vector<ECS::Entity> &order);

} // namespace Assisi::Runtime
