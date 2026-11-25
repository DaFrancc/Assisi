/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestScreenSurfaces.cpp
/// @brief A screen shown on a mesh in the world: drawn into a picture of its
/// own with its entity as its context, shown by the mesh's slot, made again
/// when its component changes and gone with its entity or its component, and
/// worked through a pointer cast from the camera onto the mesh.

#include <doctest/doctest.h>

#include <Assisi/App/ScreenSurfaces.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/TestComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/App/WorldWidgets.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Geometry/DefaultMeshes.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/ScreenDocument.hpp>
#include <Assisi/Mondrian/ScreenReader.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

using namespace Assisi;
using App::Test::TestHealth;

namespace
{

constexpr float kStep = 1.f / 60.f;
constexpr Mondrian::Extent kViewport{1280, 720};
constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 320;

/// Where the monitor stands in front of the camera, which looks down -z.
constexpr glm::dvec3 kAhead{0.0, 0.0, -4.0};
/// How large the monitor is, in metres across and up.
constexpr glm::vec3 kMonitorScale{2.f, 1.f, 1.f};

/// What the host's maker hands back for the monitor's picture.
constexpr Mondrian::TargetTexture kPicture{42};

/// How far a cast pointer may land from where the arithmetic puts it, in
/// picture pixels: the window pixel it starts from is rounded by projection.
constexpr float kCastSlack = 1.f;

/// The bar's height, in UI pixels.
constexpr float kBarHeight = 10.f;

/// The id the test's reader answers for the monitor's screen.
Core::AssetId MonitorScreenId()
{
    Core::AssetId id;
    id.bytes[0] = 0x6b;
    id.bytes[6] = 0x40;
    return id;
}

/// A bar as wide as its entity's health.
Mondrian::ScreenDocument MonitorScreen()
{
    Mondrian::ScreenDocument document;
    document.nodes.push_back(Mondrian::ScreenNode{});
    Mondrian::ScreenNode bar;
    bar.parent = 0;
    bar.style.sizing[static_cast<std::size_t>(Mondrian::Axis::Y)] = Mondrian::Sizing::Fixed(Mondrian::Px(kBarHeight));
    bar.style.background = Math::Color4<Math::ColorSpace::Srgb>{0.f, 1.f, 0.f, 1.f};
    bar.written = Mondrian::PropertyBit(Mondrian::StyleProperty::Height) |
                  Mondrian::PropertyBit(Mondrian::StyleProperty::Background);
    bar.boundAttributes.push_back(Mondrian::AttributeBinding{.length = Mondrian::Sizing::Fixed(Mondrian::Px(0.f)),
                                                             .binding = document.AddText("{TestHealth.current}"),
                                                             .attribute = Mondrian::BoundAttribute::Width});
    document.nodes.push_back(bar);
    return document;
}

/// A world with an active camera at the origin looking down -z, and a monitor
/// ahead of it: a quad showing a screen, run as a frame runs it.
struct Stage
{
    Core::EventQueue universal{Core::EventScope::Universal};
    /// Before the UI and the world, which release targets into it as they go.
    std::vector<Mondrian::TargetId> released;
    /// Before the world, so it outlives the screens the world owns.
    Mondrian::Ui ui{universal};
    App::World world{Physics::NoCollisionAssets()};
    ECS::Entity camera;
    ECS::Entity monitor;
    Mondrian::ScreenIdReader previous;

    Stage()
    {
        previous = Mondrian::SetScreenIdReader(
            [](Core::AssetId id) -> std::expected<Mondrian::ScreenDocument, Mondrian::ScreenReadError>
            {
                if (id == MonitorScreenId())
                {
                    return MonitorScreen();
                }
                return std::unexpected(Mondrian::ScreenReadError::Missing);
            });
        ui.SetTargetHost([](Mondrian::TargetId, Mondrian::Extent)
                         { return std::optional<Mondrian::TargetTexture>{kPicture}; },
                         [this](Mondrian::TargetId gone) { released.push_back(gone); });

        camera = world.scene.Create();
        (void)world.scene.Add<ECS::Transform>(camera);
        world.scene.Add<Runtime::Camera>(camera)->isActive = true;

        monitor = world.scene.Create();
        ECS::Transform *const placed = world.scene.Add<ECS::Transform>(monitor);
        placed->position = kAhead;
        placed->scale = kMonitorScale;
        world.scene.Add<Runtime::MeshRenderer>(monitor)->mesh = Core::BuiltinAssetId::Quad;
        world.scene.Add<TestHealth>(monitor)->current = 60.f;
        (void)world.scene.Add(monitor, App::ScreenSurface{.screen = MonitorScreenId(),
                                                          .width = kWidth,
                                                          .height = kHeight,
                                                          .materialSlot = 0});
        ui.ProcessInput({});
        ui.Sync(kViewport);
    }

    ~Stage() { (void)Mondrian::SetScreenIdReader(previous); }

    Stage(const Stage &) = delete;
    Stage &operator=(const Stage &) = delete;
    Stage(Stage &&) = delete;
    Stage &operator=(Stage &&) = delete;

    [[nodiscard]] App::SystemContext Context(Mondrian::Ui *shown)
    {
        App::SystemContext ctx{world, kStep, 0, nullptr, nullptr, universal, true, nullptr};
        ctx.ui = shown;
        return ctx;
    }

    void Frame(const Mondrian::UiInput &input = {})
    {
        ui.ProcessInput(input);
        App::SystemContext ctx = Context(&ui);
        App::ScreenSurfaceSystem(ctx);
        ui.Sync(kViewport);
        App::EndWorldFrame(world, universal, true);
    }

    [[nodiscard]] Mondrian::Screen *Screen()
    {
        const App::ScreenSurfaceScreen *const held = world.scene.Get<App::ScreenSurfaceScreen>(monitor);
        return held != nullptr ? App::FindScreen(world, held->screen) : nullptr;
    }

    App::ScreenSurface &Surface() { return *world.scene.GetMut<App::ScreenSurface>(monitor); }

    /// Where the camera draws @p local, a point in the monitor's own space.
    [[nodiscard]] Mondrian::Point Drawn(const glm::vec3 &local) const
    {
        const ECS::WorldMatrix *const placed = world.scene.Get<ECS::WorldMatrix>(monitor);
        const ECS::WorldMatrix *const eye = world.scene.Get<ECS::WorldMatrix>(camera);
        REQUIRE(placed != nullptr);
        REQUIRE(eye != nullptr);
        const glm::dvec3 point = glm::dvec3(ECS::MatrixFrom(*placed, glm::dvec3{0.0}) * glm::vec4(local, 1.f));
        return App::ProjectToScreen(point, *eye, *world.scene.Get<Runtime::Camera>(camera), kViewport).at;
    }

    /// How the pointer is cast onto the monitor as it stands now.
    [[nodiscard]] App::SurfaceSight Sight()
    {
        world.propagationTick = ECS::PropagateTransforms(world.scene, world.propagationTick);
        const std::optional<App::SceneView> view = App::ActiveSceneCamera(world.scene);
        REQUIRE(view.has_value());
        return App::SightOf(
            std::make_shared<const App::SurfaceGeometry>(App::SurfaceOf(Geometry::CreateUnitQuadMesh(), 0)),
            *world.scene.Get<ECS::WorldMatrix>(monitor), *view, kViewport, {.width = kWidth, .height = kHeight});
    }
};

bool Near(Mondrian::Point a, Mondrian::Point b)
{
    return std::abs(a.x - b.x) <= kCastSlack && std::abs(a.y - b.y) <= kCastSlack;
}

/// @p count indices from @p offset, drawn with slot @p slot.
Geometry::SubMesh SubMeshOf(uint32_t offset, uint32_t count, uint32_t slot)
{
    Geometry::SubMesh made;
    made.IndexOffset = offset;
    made.IndexCount = count;
    made.MaterialSlot = slot;
    return made;
}

/// Two triangles in slot 0 and one in slot 1 at the finest level, and one in
/// slot 0 at a coarser level.
Geometry::MeshData TwoSlotMesh()
{
    Geometry::MeshData mesh;
    for (int32_t at = 0; at < 4; ++at)
    {
        mesh.Vertices.push_back({{static_cast<float>(at), 0.f, 0.f}});
        mesh.Vertices.push_back({{static_cast<float>(at), 1.f, 0.f}});
        mesh.Vertices.push_back({{static_cast<float>(at) + 1.f, 0.f, 0.f}});
    }
    for (uint32_t index = 0; index < 12; ++index)
    {
        mesh.Indices.push_back(index);
    }
    mesh.SubMeshes = {SubMeshOf(0, 6, 0), SubMeshOf(6, 3, 1), SubMeshOf(9, 3, 0)};
    mesh.Lods = {Geometry::LodRange{.FirstSubMesh = 0, .SubMeshCount = 2},
                 Geometry::LodRange{.FirstSubMesh = 2, .SubMeshCount = 1}};
    return mesh;
}

} // namespace

TEST_CASE("ScreenSurfaces: a surface's screen draws into a picture of its size, reading its own entity")
{
    Stage stage;
    stage.Frame();
    stage.Frame(); // the binding reads on the sync after the screen is made
    Mondrian::Screen *const screen = stage.Screen();
    REQUIRE(screen != nullptr);
    CHECK(screen->IsShown());
    CHECK(screen->Target() != Mondrian::kWindowTarget);
    CHECK(stage.ui.TargetSize(screen->Target()) == Mondrian::Extent{kWidth, kHeight});
    // Nothing of it on the player's display, all of it in the picture.
    CHECK(stage.ui.GetDrawList().Instances().empty());
    REQUIRE(stage.ui.TargetDrawList(screen->Target()) != nullptr);
    CHECK_FALSE(stage.ui.TargetDrawList(screen->Target())->Instances().empty());
    // Not taking the keys for being shown.
    CHECK_FALSE(stage.ui.TakesInput());

    const Mondrian::LayoutNode *const root = screen->GetLayout().Get(screen->Root());
    REQUIRE(root != nullptr);
    const Mondrian::LayoutNode *const bar = screen->GetLayout().Get(screen->Tree().Get(screen->Root())->firstChild);
    REQUIRE(bar != nullptr);
    CHECK(bar->rect.width == doctest::Approx(60.f * screen->GetLayout().scale).epsilon(0.02));
}

TEST_CASE("ScreenSurfaces: the mesh's slot shows the picture the host made")
{
    Stage stage;
    stage.Frame();
    const Runtime::SurfaceTexture *const shown = stage.world.scene.Get<Runtime::SurfaceTexture>(stage.monitor);
    REQUIRE(shown != nullptr);
    CHECK(shown->materialSlot == 0);
    CHECK(shown->textureSlot == kPicture.value);
}

TEST_CASE("ScreenSurfaces: a surface's screen and picture go with its entity, its component, or a change to it")
{
    Stage stage;
    stage.Frame();
    Mondrian::Screen *const screen = stage.Screen();
    REQUIRE(screen != nullptr);
    const Mondrian::TargetId first = screen->Target();

    SUBCASE("the entity destroyed")
    {
        stage.world.scene.Destroy(stage.monitor);
        stage.world.scene.FlushDestroyed();
        stage.Frame();
        CHECK(stage.world.screenStack.empty());
        CHECK(stage.released == std::vector<Mondrian::TargetId>{first});
    }
    SUBCASE("the component taken off")
    {
        (void)stage.world.scene.Remove<App::ScreenSurface>(stage.monitor);
        stage.Frame();
        CHECK(stage.world.screenStack.empty());
        CHECK(stage.released == std::vector<Mondrian::TargetId>{first});
        CHECK_FALSE(stage.world.scene.Has<Runtime::SurfaceTexture>(stage.monitor));
        CHECK_FALSE(stage.world.scene.Has<App::ScreenSurfaceScreen>(stage.monitor));
    }
    SUBCASE("a sharper picture asked for")
    {
        stage.Surface().width = kWidth * 2;
        stage.Frame();
        CHECK(stage.released == std::vector<Mondrian::TargetId>{first});
        REQUIRE(stage.Screen() != nullptr);
        CHECK(stage.ui.TargetSize(stage.Screen()->Target()) == Mondrian::Extent{kWidth * 2, kHeight});
        CHECK(stage.world.screenStack.size() == 1);
    }
    SUBCASE("a screen file that does not load")
    {
        Core::AssetId missing;
        missing.bytes[0] = 0x01;
        stage.Surface().screen = missing;
        stage.Frame();
        CHECK(stage.world.screenStack.empty());
        CHECK_FALSE(stage.world.scene.Has<Runtime::SurfaceTexture>(stage.monitor));
        // Not tried again every frame.
        stage.Frame();
        CHECK(stage.world.scene.Has<App::ScreenSurfaceScreen>(stage.monitor));
    }
}

TEST_CASE("ScreenSurfaces: a headless host makes no screen")
{
    Stage stage;
    App::SystemContext ctx = stage.Context(nullptr);
    App::ScreenSurfaceSystem(ctx);
    CHECK(stage.world.screenStack.empty());
    CHECK_FALSE(App::UseScreenSurface(ctx, stage.monitor));
}

TEST_CASE("ScreenSurfaces: a slot's triangles are its own, at the finest level")
{
    const Geometry::MeshData mesh = TwoSlotMesh();
    CHECK(App::SurfaceOf(mesh, 0).triangles.size() == 2);
    CHECK(App::SurfaceOf(mesh, 1).triangles.size() == 1);
    CHECK(App::SurfaceOf(mesh, 2).triangles.empty());
    // A mesh with no submeshes is all slot 0.
    CHECK(App::SurfaceOf(Geometry::CreateUnitQuadMesh(), 0).triangles.size() == 2);
    CHECK(App::SurfaceOf(Geometry::CreateUnitQuadMesh(), 1).triangles.empty());
}

TEST_CASE("ScreenSurfaces: a ray meets a surface at its texture coordinates, from either side, and misses beside it")
{
    const App::SurfaceGeometry quad = App::SurfaceOf(Geometry::CreateUnitQuadMesh(), 0);
    const std::optional<glm::vec2> middle = App::SurfaceHit(quad, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f});
    REQUIRE(middle.has_value());
    CHECK(middle->x == doctest::Approx(0.5f));
    CHECK(middle->y == doctest::Approx(0.5f));

    // The top left of the quad is the top left of its picture.
    const std::optional<glm::vec2> topLeft = App::SurfaceHit(quad, {-0.4f, 0.4f, -1.f}, {0.f, 0.f, 1.f});
    REQUIRE(topLeft.has_value());
    CHECK(topLeft->x == doctest::Approx(0.1f));
    CHECK(topLeft->y == doctest::Approx(0.1f));

    CHECK_FALSE(App::SurfaceHit(quad, {0.6f, 0.f, 1.f}, {0.f, 0.f, -1.f}).has_value());
    // Behind where the ray starts.
    CHECK_FALSE(App::SurfaceHit(quad, {0.f, 0.f, -1.f}, {0.f, 0.f, -1.f}).has_value());
}

TEST_CASE("ScreenSurfaces: the window pointer is cast onto the surface where the camera drew it")
{
    Stage stage;
    stage.Frame();

    SUBCASE("facing the camera")
    {
    }
    SUBCASE("turned and tilted")
    {
        stage.world.scene.GetMut<ECS::Transform>(stage.monitor)->rotation =
            glm::angleAxis(0.5f, glm::normalize(glm::vec3{0.3f, 1.f, 0.2f}));
    }
    const App::SurfaceSight sight = stage.Sight();

    const std::optional<Mondrian::Point> middle = App::PointOnSurface(sight, stage.Drawn({0.f, 0.f, 0.f}));
    REQUIRE(middle.has_value());
    CHECK(Near(*middle, {.x = kWidth * 0.5f, .y = kHeight * 0.5f}));

    const std::optional<Mondrian::Point> nearCorner = App::PointOnSurface(sight, stage.Drawn({-0.4f, 0.4f, 0.f}));
    REQUIRE(nearCorner.has_value());
    CHECK(Near(*nearCorner, {.x = kWidth * 0.1f, .y = kHeight * 0.1f}));

    const std::optional<Mondrian::Point> farCorner = App::PointOnSurface(sight, stage.Drawn({0.4f, -0.4f, 0.f}));
    REQUIRE(farCorner.has_value());
    CHECK(Near(*farCorner, {.x = kWidth * 0.9f, .y = kHeight * 0.9f}));

    CHECK_FALSE(App::PointOnSurface(sight, stage.Drawn({0.8f, 0.f, 0.f})).has_value());
}

TEST_CASE("ScreenSurfaces: using a surface hands it the keys and the pointer, and leaving hands them back")
{
    Stage stage;
    stage.Frame();
    Mondrian::Screen *const screen = stage.Screen();
    REQUIRE(screen != nullptr);
    App::SystemContext ctx = stage.Context(&stage.ui);
    CHECK(App::UseScreenSurface(ctx, stage.monitor));
    CHECK(stage.ui.InUse() == screen);
    CHECK(stage.ui.InputScreen() == screen);
    CHECK(stage.ui.TakesInput());

    // The frame after casts the pointer: over the monitor it is over its
    // screen, and beside it over nothing on it.
    stage.Frame();
    Mondrian::UiInput over;
    over.grant = Mondrian::InputGrant::Everything;
    over.pointer = stage.Drawn({0.f, 0.f, 0.f});
    stage.Frame(over);
    CHECK(stage.ui.GetInteraction().pointed);
    Mondrian::UiInput beside = over;
    beside.pointer = stage.Drawn({1.f, 0.f, 0.f});
    stage.Frame(beside);
    CHECK_FALSE(stage.ui.GetInteraction().pointed);

    App::LeaveScreenSurface(ctx);
    CHECK(stage.ui.InUse() == nullptr);
    CHECK_FALSE(stage.ui.TakesInput());
    CHECK_FALSE(App::UseScreenSurface(ctx, stage.camera));
}
