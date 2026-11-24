/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/WorldWidgets.hpp>

#include <Assisi/App/SceneCamera.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Physics/PhysicsWorld.hpp>
#include <Assisi/Runtime/Camera.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Assisi::App
{
namespace
{

/// A clip-space w at or below this is not in front of the camera: a point on
/// the camera's own plane has no place on screen.
constexpr float kInFrontW = 1e-6f;

/// How far across a viewport its middle is, in clip space's terms.
constexpr float kHalf = 0.5f;

/// How much a widget at full strength shows, and at its full size.
constexpr float kWhole = 1.f;

/// How far from the middle normalised device coordinates reach at the edge of
/// the viewport.
constexpr float kNdcEdge = 1.f;

/// The narrowest fade a widget's two fade distances are taken to span, in
/// metres, so two equal distances fade at once rather than divide by nothing.
constexpr float kMinFadeSpan = 1e-3f;

/// The edge direction for a point straight behind the camera, which has no
/// sideways direction of its own: the bottom of the screen, behind the viewer.
constexpr glm::vec2 kStraightBehind{0.f, -1.f};

/// @p ndc, from -1 to 1 up and right, as device pixels down and right in a
/// @p width by @p height viewport.
Mondrian::Point ToPixels(glm::vec2 ndc, float width, float height)
{
    return {.x = ((ndc.x * kHalf) + kHalf) * width, .y = (kHalf - (ndc.y * kHalf)) * height};
}

/// How much of the widget shows and how large, at @p distance metres.
struct WidgetLook
{
    float opacity = kWhole;
    float scale = kWhole;
};

WidgetLook LookAt(const WorldWidget &widget, float distance)
{
    WidgetLook look;
    if (widget.fadeFar > 0.f)
    {
        const float span = std::max(widget.fadeFar - widget.fadeNear, kMinFadeSpan);
        look.opacity = std::clamp((widget.fadeFar - distance) / span, 0.f, kWhole);
    }
    if (widget.scaleDistance > 0.f && distance > widget.scaleDistance)
    {
        look.scale = widget.scaleDistance / distance;
    }
    return look;
}

/// Whether @p entity is @p holder or held by it, however deeply.
bool IsWithin(const ECS::Scene &scene, ECS::Entity entity, ECS::Entity holder)
{
    for (ECS::Entity at = entity; at != ECS::NullEntity;)
    {
        if (at == holder)
        {
            return true;
        }
        const ECS::Parent *const parent = scene.Get<ECS::Parent>(at);
        at = parent != nullptr ? parent->parent : ECS::NullEntity;
    }
    return false;
}

/// Whether a collider stands between @p eye and @p point, other than the
/// widget's own @p entity and whatever holds the camera @p viewer: a camera
/// inside its character's capsule would otherwise always be blocked.
bool IsBlocked(const World &world, const glm::dvec3 &eye, const glm::dvec3 &point, ECS::Entity entity,
               ECS::Entity viewer)
{
    std::vector<Physics::QueryHit> hits;
    world.physics.CastRayAll(eye, glm::vec3(point - eye),
                             Physics::CollisionFilter{}.Without(Physics::CollisionChannel::Trigger), entity, hits);
    return std::ranges::any_of(hits,
                               [&world, entity, viewer](const Physics::QueryHit &hit)
                               {
                                   return !IsWithin(world.scene, hit.entity, entity) &&
                                          !IsWithin(world.scene, viewer, hit.entity);
                               });
}

/// Whether a widget with @p widget's settings is hidden for being behind the
/// camera, whatever else holds.
bool HiddenBehind(const WorldWidget &widget, const ProjectedPoint &projected)
{
    return !projected.inFront && widget.offScreen == WorldWidgetOffScreen::Hides;
}

/// Whether a widget with @p widget's settings shows, its entity having
/// landed at @p projected and being @p blocked or not.
bool Shows(const WorldWidget &widget, const ProjectedPoint &projected, bool blocked)
{
    if (HiddenBehind(widget, projected))
    {
        return false;
    }
    switch (widget.blocked)
    {
    case WorldWidgetBlocked::Hides:
        return !blocked;
    case WorldWidgetBlocked::Shows:
        return blocked;
    case WorldWidgetBlocked::Ignored:
    case WorldWidgetBlocked::Count_:
        break;
    }
    return true;
}

/// Shows @p screen when @p shows, hides it otherwise, touching it only when
/// that changes.
void ShowIf(Mondrian::Screen &screen, bool shows)
{
    if (shows && !screen.IsShown())
    {
        screen.Show();
    }
    else if (!shows && screen.IsShown())
    {
        screen.Hide();
    }
}

/// Takes away the screens of entities whose WorldWidget was taken off.
void RemoveUnwanted(World &world)
{
    std::vector<ECS::Entity> unwanted;
    for (auto [entity, held] : world.scene.Query<WorldWidgetScreen>())
    {
        if (!world.scene.Has<WorldWidget>(entity))
        {
            unwanted.push_back(entity);
        }
    }
    for (const ECS::Entity entity : unwanted)
    {
        if (Mondrian::Screen *const screen = FindScreen(world, world.scene.Get<WorldWidgetScreen>(entity)->screen))
        {
            RemoveScreen(world, *screen);
        }
        (void)world.scene.Remove<WorldWidgetScreen>(entity);
    }
}

/// The screen @p entity's @p widget asks for, made the first time and again
/// when the widget names another file, or null when it names none or the file
/// did not load. A file that did not load is not tried again until the widget
/// names another, so its error is logged once.
Mondrian::Screen *ScreenFor(SystemContext &ctx, ECS::Entity entity, const WorldWidget &widget)
{
    World &world = ctx.world;
    if (const WorldWidgetScreen *const held = world.scene.Get<WorldWidgetScreen>(entity))
    {
        if (held->from == widget.screen)
        {
            return FindScreen(world, held->screen);
        }
        if (Mondrian::Screen *const stale = FindScreen(world, held->screen))
        {
            RemoveScreen(world, *stale);
        }
        (void)world.scene.Remove<WorldWidgetScreen>(entity);
    }
    if (widget.screen.IsNil())
    {
        return nullptr;
    }
    Mondrian::Screen *const screen = LoadScreen(world, *ctx.ui, widget.screen);
    if (screen == nullptr)
    {
        (void)world.scene.Add(entity, WorldWidgetScreen{.screen = {}, .from = widget.screen});
        return nullptr;
    }
    const ScreenId id = IdOf(world, *screen);
    screen->SetContext(ScreenEntity(entity));
    // Taken away with the entity however it goes, even before the screen has
    // drawn once.
    SetScreenOwner(world, id, entity);
    (void)world.scene.Add(entity, WorldWidgetScreen{.screen = id, .from = widget.screen});
    return screen;
}

/// Places, shows or hides @p entity's widget for this frame, seen through
/// @p view in @p viewport.
void PlaceWidget(SystemContext &ctx, ECS::Entity entity, const WorldWidget &widget, const SceneView &view,
                 Mondrian::Extent viewport)
{
    Mondrian::Screen *const screen = ScreenFor(ctx, entity, widget);
    if (screen == nullptr)
    {
        return;
    }
    const ECS::WorldMatrix *const placed = ctx.world.scene.Get<ECS::WorldMatrix>(entity);
    if (placed == nullptr || viewport.width == 0 || viewport.height == 0)
    {
        ShowIf(*screen, false);
        return;
    }
    const glm::dvec3 point = ECS::WorldPositionOf(*placed) + glm::dvec3(widget.offset);
    const ProjectedPoint projected = ProjectToScreen(point, view.world, view.camera, viewport);
    const glm::dvec3 eye = ECS::WorldPositionOf(view.world);
    // Only asked when the answer matters: a ray is the one cost here that
    // grows with the scene.
    const bool blocked = widget.blocked != WorldWidgetBlocked::Ignored && !HiddenBehind(widget, projected) &&
                         IsBlocked(ctx.world, eye, point, entity, view.entity);
    const WidgetLook look = LookAt(widget, static_cast<float>(glm::length(point - eye)));
    const bool shows = Shows(widget, projected, blocked) && look.opacity > 0.f;
    ShowIf(*screen, shows);
    if (shows)
    {
        screen->SetAnchor(projected.at, widget.offScreen == WorldWidgetOffScreen::StaysOnEdge);
        screen->SetOpacity(look.opacity);
        screen->SetScale(look.scale);
    }
}

} // namespace

ProjectedPoint ProjectToScreen(const glm::dvec3 &point, const ECS::WorldMatrix &cameraWorld,
                               const Runtime::Camera &camera, Mondrian::Extent viewport)
{
    const float width = static_cast<float>(viewport.width);
    const float height = static_cast<float>(viewport.height);
    // Measured from the camera, as the scene is drawn, so a point a long way
    // from the world's origin keeps its precision.
    const glm::dvec3 eye = ECS::WorldPositionOf(cameraWorld);
    const glm::mat4 view = Runtime::ViewMatrix(ECS::MatrixFrom(cameraWorld, eye));
    const glm::mat4 projection =
        Runtime::ProjectionMatrix(camera, Runtime::AspectRatio(static_cast<int32_t>(viewport.width),
                                                               static_cast<int32_t>(viewport.height)));
    const glm::vec4 clip = projection * view * glm::vec4(glm::vec3(point - eye), 1.f);

    ProjectedPoint projected;
    if (clip.w > kInFrontW)
    {
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        projected.at = ToPixels(ndc, width, height);
        projected.inFront = true;
        projected.onScreen = std::abs(ndc.x) <= kNdcEdge && std::abs(ndc.y) <= kNdcEdge;
        return projected;
    }
    // Behind the camera, dividing by w would mirror the point through the
    // middle of the screen. Its direction is the sign of its own x and y, and
    // the edge in that direction is where a widget kept on screen points.
    glm::vec2 direction{clip.x, clip.y};
    const float reach = std::max(std::abs(direction.x), std::abs(direction.y));
    direction = reach > 0.f ? direction / reach : kStraightBehind;
    projected.at = ToPixels(direction, width, height);
    return projected;
}

void WorldWidgetSystem(SystemContext &ctx)
{
    // PresentWorld has done this already; again here so the system needs
    // nothing else run before it.
    RemoveOrphanedScreens(ctx.world);
    if (ctx.ui == nullptr) // a headless host shows nothing
    {
        return;
    }
    World &world = ctx.world;
    RemoveUnwanted(world);

    // Whatever an earlier Ui system moved, such as a camera rig, placed before
    // anything is projected from it.
    world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
    const std::optional<SceneView> view = ActiveSceneCamera(world.scene);

    // Gathered first: making a screen adds a component, which a query being
    // walked must not see.
    std::vector<ECS::Entity> widgets;
    for (auto [entity, widget] : world.scene.Query<WorldWidget>())
    {
        widgets.push_back(entity);
    }
    for (const ECS::Entity entity : widgets)
    {
        const WorldWidget widget = *world.scene.Get<WorldWidget>(entity);
        if (!view)
        {
            // Nothing to see it through: shown nowhere.
            if (Mondrian::Screen *const screen = ScreenFor(ctx, entity, widget))
            {
                ShowIf(*screen, false);
            }
            continue;
        }
        PlaceWidget(ctx, entity, widget, *view, ctx.ui->Viewport());
    }
}

} // namespace Assisi::App
