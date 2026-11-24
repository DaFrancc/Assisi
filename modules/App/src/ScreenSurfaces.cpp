/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/ScreenSurfaces.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Render/AssetCache.hpp>
#include <Assisi/Render/AssetSource.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

namespace Assisi::App
{
namespace
{

/// A ray this close to a triangle's plane, by the determinant of its edges,
/// runs along it rather than through it.
constexpr float kParallel = 1e-8f;

/// How far ahead of its origin a ray must meet a triangle to count, in the
/// surface's own units, so a pointer cast from on the surface does not meet
/// the surface it starts on.
constexpr float kAhead = 1e-6f;

/// A barycentric weight this far below zero still counts as inside, so a ray
/// along the edge two triangles share meets one of them.
constexpr float kEdgeSlack = 1e-6f;

/// The direction a camera looks along its own space, as Runtime::ViewMatrix
/// builds it.
constexpr float kForward = -1.f;

/// The triangles from index @p first, @p count indices long, added to
/// @p surface. Indices naming no vertex are skipped.
void AddTriangles(const Geometry::MeshData &mesh, std::size_t first, std::size_t count, SurfaceGeometry &surface)
{
    const std::size_t end = std::min(first + count, mesh.Indices.size());
    for (std::size_t at = first; at + 2 < end; at += 3)
    {
        SurfaceTriangle triangle;
        bool whole = true;
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t index = mesh.Indices[at + corner];
            if (index >= mesh.Vertices.size())
            {
                whole = false;
                break;
            }
            triangle.corners[corner] = mesh.Vertices[index].Position;
            triangle.uvs[corner] = mesh.Vertices[index].TextureCoordinates;
        }
        if (whole)
        {
            surface.triangles.push_back(triangle);
        }
    }
}

/// The surface @p mesh's slot @p slot draws, read from the mesh as it is
/// stored: a built-in is built, a file is read through the installed asset
/// source. Null, logged, when it cannot be read.
std::shared_ptr<const SurfaceGeometry> GeometryFor(const Core::AssetId &mesh, uint32_t slot)
{
    // A nil mesh draws as the cube, so it is cast onto as one.
    const Core::AssetId drawn = mesh.IsNil() ? Core::BuiltinAssetId::Cube : mesh;
    if (std::optional<Geometry::MeshData> primitive = Render::AssetCache::PrimitiveMeshData(drawn))
    {
        return std::make_shared<const SurfaceGeometry>(SurfaceOf(*primitive, slot));
    }
    const Render::AssetSource *source = Render::GetAssetSource();
    if (source == nullptr)
    {
        return nullptr;
    }
    std::expected<Geometry::CookedMesh, Render::AssetLoadError> cooked = source->LoadMesh(drawn);
    if (!cooked)
    {
        Core::Log::Error("ScreenSurfaces: the mesh '{}' could not be read ({}); its screen cannot be pointed at.",
                         source->Describe(drawn), Render::ToString(cooked.error()));
        return nullptr;
    }
    return std::make_shared<const SurfaceGeometry>(SurfaceOf(cooked->mesh, slot));
}

/// Whether what @p held was made from is what @p surface, on @p mesh, asks for
/// now.
bool StillMatches(const ScreenSurfaceScreen &held, const ScreenSurface &surface, const Core::AssetId &mesh)
{
    return held.from == surface.screen && held.width == surface.width && held.height == surface.height &&
           held.materialSlot == surface.materialSlot && held.mesh == mesh;
}

/// Takes away what was made for @p entity's surface: its screen with its
/// picture, and the slot showing it, which shows its own material again.
void TearDown(World &world, ECS::Entity entity)
{
    if (const ScreenSurfaceScreen *const held = world.scene.Get<ScreenSurfaceScreen>(entity))
    {
        if (Mondrian::Screen *const screen = FindScreen(world, held->screen))
        {
            RemoveScreen(world, *screen);
        }
    }
    (void)world.scene.Remove<ScreenSurfaceScreen>(entity);
    if (world.scene.Remove<Runtime::SurfaceTexture>(entity) == ECS::RemoveResult::Removed)
    {
        const std::array<ECS::Entity, 1> changed{entity};
        ResolveEntityAssets(world, changed);
    }
}

/// Takes away what was made for entities whose ScreenSurface was taken off.
void RemoveUnwanted(World &world)
{
    std::vector<ECS::Entity> unwanted;
    for (auto [entity, held] : world.scene.Query<ScreenSurfaceScreen>())
    {
        if (!world.scene.Has<ScreenSurface>(entity))
        {
            unwanted.push_back(entity);
        }
    }
    for (const ECS::Entity entity : unwanted)
    {
        TearDown(world, entity);
    }
}

/// Makes @p entity's screen for @p surface: loaded with the entity as its
/// context, drawn into a picture of its own that goes with it, and shown by
/// the slot. A file that does not load is not tried again until the component
/// changes, so its error is logged once.
void Make(SystemContext &ctx, ECS::Entity entity, const ScreenSurface &surface, const Core::AssetId &mesh)
{
    World &world = ctx.world;
    ScreenSurfaceScreen made{.geometry = nullptr,
                             .screen = {},
                             .from = surface.screen,
                             .mesh = mesh,
                             .width = surface.width,
                             .height = surface.height,
                             .materialSlot = surface.materialSlot};
    Mondrian::Screen *const screen = surface.screen.IsNil() ? nullptr : LoadScreen(world, *ctx.ui, surface.screen);
    if (screen == nullptr || surface.width == 0 || surface.height == 0)
    {
        if (screen != nullptr)
        {
            RemoveScreen(world, *screen);
        }
        (void)world.scene.Add(entity, std::move(made));
        return;
    }
    made.screen = IdOf(world, *screen);
    screen->SetContext(ScreenEntity(entity));
    // Taken away with the entity however it goes, its picture with it.
    SetScreenOwner(world, made.screen, entity);
    const Mondrian::TargetId target =
        DrawScreenInto(world, *ctx.ui, made.screen, {.width = surface.width, .height = surface.height});
    screen->Show();
    made.geometry = GeometryFor(mesh, surface.materialSlot);
    (void)world.scene.Add(entity, std::move(made));

    // A host that makes no pictures, such as a test, still has the screen.
    if (const std::optional<Mondrian::TargetTexture> texture = ctx.ui->GetTargetTexture(target))
    {
        (void)world.scene.Add(
            entity, Runtime::SurfaceTexture{.materialSlot = surface.materialSlot, .textureSlot = texture->value});
        const std::array<ECS::Entity, 1> changed{entity};
        ResolveEntityAssets(world, changed);
    }
}

/// Casts the pointer onto @p entity's surface while its screen is the one used.
void AimPointer(SystemContext &ctx, ECS::Entity entity, const ScreenSurfaceScreen &held, const SceneView &view)
{
    Mondrian::Ui &ui = *ctx.ui;
    Mondrian::Screen *const screen = FindScreen(ctx.world, held.screen);
    const ECS::WorldMatrix *const placed = ctx.world.scene.Get<ECS::WorldMatrix>(entity);
    if (screen == nullptr || ui.InUse() != screen || placed == nullptr || held.geometry == nullptr)
    {
        return;
    }
    SurfaceSight sight =
        SightOf(held.geometry, *placed, view, ui.Viewport(), {.width = held.width, .height = held.height});
    ui.SetTargetPointer(screen->Target(), [sight = std::move(sight)](Mondrian::Point pointer)
                        { return PointOnSurface(sight, pointer); });
}

} // namespace

SurfaceGeometry SurfaceOf(const Geometry::MeshData &mesh, uint32_t slot)
{
    SurfaceGeometry surface;
    if (mesh.SubMeshes.empty())
    {
        if (slot == 0)
        {
            AddTriangles(mesh, 0, mesh.Indices.size(), surface);
        }
        return surface;
    }
    // The finest level: what is drawn up close, where a screen is worked.
    std::size_t first = 0;
    std::size_t count = mesh.SubMeshes.size();
    if (!mesh.Lods.empty())
    {
        first = mesh.Lods[0].FirstSubMesh;
        count = mesh.Lods[0].SubMeshCount;
    }
    for (std::size_t at = first; at < first + count && at < mesh.SubMeshes.size(); ++at)
    {
        const Geometry::SubMesh &sub = mesh.SubMeshes[at];
        if (sub.MaterialSlot == slot)
        {
            AddTriangles(mesh, sub.IndexOffset, sub.IndexCount, surface);
        }
    }
    return surface;
}

std::optional<glm::vec2> SurfaceHit(const SurfaceGeometry &surface, const glm::vec3 &origin,
                                    const glm::vec3 &direction)
{
    // Moller and Trumbore's test, "Fast, Minimum Storage Ray/Triangle
    // Intersection" (1997), with both faces counting.
    float nearest = std::numeric_limits<float>::infinity();
    std::optional<glm::vec2> hit;
    for (const SurfaceTriangle &triangle : surface.triangles)
    {
        const glm::vec3 edgeA = triangle.corners[1] - triangle.corners[0];
        const glm::vec3 edgeB = triangle.corners[2] - triangle.corners[0];
        const glm::vec3 across = glm::cross(direction, edgeB);
        const float determinant = glm::dot(edgeA, across);
        if (std::abs(determinant) < kParallel)
        {
            continue;
        }
        const float inverse = 1.f / determinant;
        const glm::vec3 fromCorner = origin - triangle.corners[0];
        const float u = glm::dot(fromCorner, across) * inverse;
        if (u < -kEdgeSlack || u > 1.f + kEdgeSlack)
        {
            continue;
        }
        const glm::vec3 up = glm::cross(fromCorner, edgeA);
        const float v = glm::dot(direction, up) * inverse;
        if (v < -kEdgeSlack || u + v > 1.f + kEdgeSlack)
        {
            continue;
        }
        const float distance = glm::dot(edgeB, up) * inverse;
        if (distance <= kAhead || distance >= nearest)
        {
            continue;
        }
        nearest = distance;
        hit = ((1.f - u - v) * triangle.uvs[0]) + (u * triangle.uvs[1]) + (v * triangle.uvs[2]);
    }
    return hit;
}

SurfaceSight SightOf(std::shared_ptr<const SurfaceGeometry> geometry, const ECS::WorldMatrix &surfaceWorld,
                     const SceneView &view, Mondrian::Extent viewport, Mondrian::Extent picture)
{
    // Measured from the camera, as the scene is drawn, so a monitor a long way
    // from the world's origin is cast onto as precisely as one beside it.
    const glm::dvec3 eye = ECS::WorldPositionOf(view.world);
    const glm::mat4 viewMatrix = Runtime::ViewMatrix(ECS::MatrixFrom(view.world, eye));
    const glm::mat4 surfaceInView = ECS::MatrixFrom(surfaceWorld, eye);

    const float aspect =
        Runtime::AspectRatio(static_cast<int32_t>(viewport.width), static_cast<int32_t>(viewport.height));

    SurfaceSight sight;
    sight.geometry = std::move(geometry);
    sight.cameraToSurface = glm::inverse(surfaceInView) * glm::inverse(viewMatrix);
    sight.inverseProjection = glm::inverse(Runtime::ProjectionMatrix(view.camera, aspect));
    sight.viewport = viewport;
    sight.picture = picture;
    return sight;
}

std::optional<Mondrian::Point> PointOnSurface(const SurfaceSight &sight, Mondrian::Point pointer)
{
    if (sight.geometry == nullptr || sight.viewport.width == 0 || sight.viewport.height == 0)
    {
        return std::nullopt;
    }
    const float width = static_cast<float>(sight.viewport.width);
    const float height = static_cast<float>(sight.viewport.height);
    const float ndcX = ((2.f * pointer.x) / width) - 1.f;
    const float ndcY = 1.f - ((2.f * pointer.y) / height);
    // The pointer's direction in the camera's space: the projection undone
    // across, and the camera's own forward along.
    glm::vec4 along = sight.inverseProjection * glm::vec4(ndcX, ndcY, kForward, 1.f);
    along.z = kForward;
    along.w = 0.f;

    const glm::vec3 origin = glm::vec3(sight.cameraToSurface * glm::vec4(0.f, 0.f, 0.f, 1.f));
    const glm::vec3 direction = glm::vec3(sight.cameraToSurface * along);
    const std::optional<glm::vec2> uv = SurfaceHit(*sight.geometry, origin, direction);
    if (!uv)
    {
        return std::nullopt;
    }
    return Mondrian::Point{.x = uv->x * static_cast<float>(sight.picture.width),
                           .y = uv->y * static_cast<float>(sight.picture.height)};
}

bool UseScreenSurface(SystemContext &ctx, ECS::Entity entity)
{
    if (ctx.ui == nullptr)
    {
        return false;
    }
    const ScreenSurfaceScreen *const held = ctx.world.scene.Get<ScreenSurfaceScreen>(entity);
    Mondrian::Screen *const screen = held != nullptr ? FindScreen(ctx.world, held->screen) : nullptr;
    if (screen == nullptr)
    {
        return false;
    }
    ctx.ui->Use(*screen);
    return ctx.ui->InUse() == screen;
}

void LeaveScreenSurface(SystemContext &ctx)
{
    if (ctx.ui != nullptr)
    {
        ctx.ui->StopUsing();
    }
}

void ScreenSurfaceSystem(SystemContext &ctx)
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

    // Gathered first: making a screen adds components, which a query being
    // walked must not see.
    std::vector<ECS::Entity> surfaces;
    for (auto [entity, surface] : world.scene.Query<ScreenSurface>())
    {
        surfaces.push_back(entity);
    }
    for (const ECS::Entity entity : surfaces)
    {
        const ScreenSurface surface = *world.scene.Get<ScreenSurface>(entity);
        const Runtime::MeshRenderer *const meshRenderer = world.scene.Get<Runtime::MeshRenderer>(entity);
        const Core::AssetId mesh = meshRenderer != nullptr ? meshRenderer->mesh : Core::AssetId{};
        const ScreenSurfaceScreen *const held = world.scene.Get<ScreenSurfaceScreen>(entity);
        if (held != nullptr && StillMatches(*held, surface, mesh))
        {
            continue;
        }
        if (held != nullptr)
        {
            TearDown(world, entity);
        }
        Make(ctx, entity, surface, mesh);
    }

    // The pointer for the frame after this one, cast from where the camera is
    // now: what the player sees when they move it.
    if (ctx.ui->InUse() == nullptr)
    {
        return;
    }
    world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
    const std::optional<SceneView> view = ActiveSceneCamera(world.scene);
    if (!view)
    {
        return;
    }
    for (auto [entity, held] : world.scene.Query<ScreenSurfaceScreen>())
    {
        AimPointer(ctx, entity, held, *view);
    }
}

} // namespace Assisi::App
