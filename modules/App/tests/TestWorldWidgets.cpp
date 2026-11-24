/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestWorldWidgets.cpp
/// @brief A screen that follows an entity: placed where the entity is drawn,
/// following it as it moves, reading it through bindings, hiding behind the
/// camera or kept on the edge, faded and shrunk with distance, shown or hidden
/// by what stands in the way, and gone with its entity or its component.

#include <doctest/doctest.h>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/TestComponents.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/App/WorldFrame.hpp>
#include <Assisi/App/WorldWidgets.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>
#include <Assisi/ECS/Hierarchy.hpp>
#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/ScreenDocument.hpp>
#include <Assisi/Mondrian/ScreenReader.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>

using namespace Assisi;
using App::Test::TestHealth;

namespace
{

constexpr float kStep = 1.f / 60.f;
constexpr Mondrian::Extent kViewport{1280, 720};
constexpr float kMiddleX = 640.f;
constexpr float kMiddleY = 360.f;

/// How far a placed edge may sit from where the arithmetic puts it, in device
/// pixels: layout snaps every edge to a whole one.
constexpr float kSnap = 1.f;

/// Where the nameplate stands in front of the camera, which looks down -z.
constexpr glm::dvec3 kAhead{0.0, 0.0, -10.0};
/// Over the entity's position, as a nameplate over a head.
constexpr glm::vec3 kOverHead{0.f, 1.f, 0.f};
/// The bar's height, in UI pixels.
constexpr float kBarHeight = 10.f;

/// The id the test's reader answers for the nameplate.
Core::AssetId NameplateId()
{
    Core::AssetId id;
    id.bytes[0] = 0x5a;
    id.bytes[6] = 0x40;
    return id;
}

/// A nameplate: a bar as wide as its entity's health, its bottom middle on the
/// point it is placed at.
Mondrian::ScreenDocument Nameplate()
{
    Mondrian::ScreenDocument document;
    Mondrian::ScreenNode root;
    document.nodes.push_back(root);
    document.attach = {Mondrian::Alignment::Center, Mondrian::Alignment::End};

    Mondrian::ScreenNode bar;
    bar.parent = 0;
    bar.style.sizing[static_cast<std::size_t>(Mondrian::Axis::Y)] = Mondrian::Sizing::Fixed(Mondrian::Px(kBarHeight));
    bar.written = Mondrian::PropertyBit(Mondrian::StyleProperty::Height);
    bar.boundAttributes.push_back(Mondrian::AttributeBinding{.size = Mondrian::Sizing::Fixed(Mondrian::Px(0.f)),
                                                             .binding = document.AddText("{TestHealth.current}"),
                                                             .attribute = Mondrian::BoundAttribute::Width});
    document.nodes.push_back(bar);
    return document;
}

/// A world with an active camera at the origin looking down -z, and one entity
/// ahead of it wearing a nameplate, run as a frame runs it.
struct Stage
{
    Core::EventQueue universal{Core::EventScope::Universal};
    /// Before the world, so it outlives the screens the world owns.
    Mondrian::Ui ui{universal};
    App::World world{Physics::NoCollisionAssets()};
    ECS::Entity camera;
    ECS::Entity entity;
    Mondrian::ScreenIdReader previous;

    Stage()
    {
        previous = Mondrian::SetScreenIdReader(
            [](Core::AssetId id) -> std::expected<Mondrian::ScreenDocument, Mondrian::ScreenReadError>
            {
                if (id == NameplateId())
                {
                    return Nameplate();
                }
                return std::unexpected(Mondrian::ScreenReadError::Missing);
            });
        camera = world.scene.Create();
        (void)world.scene.Add<ECS::Transform>(camera);
        world.scene.Add<Runtime::Camera>(camera)->isActive = true;

        entity = world.scene.Create();
        world.scene.Add<ECS::Transform>(entity)->position = kAhead;
        world.scene.Add<TestHealth>(entity)->current = 60.f;
        (void)world.scene.Add(entity, App::WorldWidget{.screen = NameplateId(), .offset = kOverHead});
        // Once, so the system has a viewport to place against.
        ui.ProcessInput({});
        ui.Sync(kViewport);
    }

    ~Stage() { (void)Mondrian::SetScreenIdReader(previous); }

    Stage(const Stage &) = delete;
    Stage &operator=(const Stage &) = delete;
    Stage(Stage &&) = delete;
    Stage &operator=(Stage &&) = delete;

    void Frame()
    {
        ui.ProcessInput({});
        App::SystemContext ctx{world, kStep, 0, nullptr, nullptr, universal, true, nullptr};
        ctx.ui = &ui;
        App::WorldWidgetSystem(ctx);
        ui.Sync(kViewport);
        App::EndWorldFrame(world, universal, true);
    }

    App::WorldWidget &Widget() { return *world.scene.GetMut<App::WorldWidget>(entity); }

    [[nodiscard]] Mondrian::Screen *Screen()
    {
        const App::WorldWidgetScreen *const held = world.scene.Get<App::WorldWidgetScreen>(entity);
        return held != nullptr ? App::FindScreen(world, held->screen) : nullptr;
    }

    /// Where the screen's root was drawn this frame.
    [[nodiscard]] Mondrian::Rect Root()
    {
        Mondrian::Screen *const screen = Screen();
        REQUIRE(screen != nullptr);
        const Mondrian::LayoutNode *const root = screen->GetLayout().Get(screen->Root());
        REQUIRE(root != nullptr);
        return root->rect;
    }

    void MoveEntity(const glm::dvec3 &to) { world.scene.GetMut<ECS::Transform>(entity)->position = to; }

    [[nodiscard]] App::ProjectedPoint Projected(const glm::dvec3 &point) const
    {
        const ECS::WorldMatrix *const placed = world.scene.Get<ECS::WorldMatrix>(camera);
        REQUIRE(placed != nullptr);
        return App::ProjectToScreen(point, *placed, *world.scene.Get<Runtime::Camera>(camera), kViewport);
    }
};

} // namespace

TEST_CASE("WorldWidgets: a point lands where the camera sees it, and one behind it on the edge it lies towards")
{
    Stage stage;
    stage.Frame(); // places the camera

    const App::ProjectedPoint ahead = stage.Projected(kAhead);
    CHECK(ahead.inFront);
    CHECK(ahead.onScreen);
    CHECK(ahead.at.x == doctest::Approx(kMiddleX));
    CHECK(ahead.at.y == doctest::Approx(kMiddleY));

    CHECK(stage.Projected(kAhead + glm::dvec3{0.0, 2.0, 0.0}).at.y < kMiddleY);
    CHECK(stage.Projected(kAhead + glm::dvec3{2.0, 0.0, 0.0}).at.x > kMiddleX);

    const App::ProjectedPoint behindRight = stage.Projected(glm::dvec3{3.0, 0.0, 10.0});
    CHECK_FALSE(behindRight.inFront);
    CHECK_FALSE(behindRight.onScreen);
    CHECK(behindRight.at.x == doctest::Approx(static_cast<float>(kViewport.width)));
}

TEST_CASE("WorldWidgets: a nameplate sits over its entity and follows it the frame it moves")
{
    Stage stage;
    stage.Frame();
    REQUIRE(stage.Screen() != nullptr);
    CHECK(stage.Screen()->IsShown());

    // Its bottom middle on the point over the entity's head.
    const App::ProjectedPoint head = stage.Projected(kAhead + glm::dvec3(kOverHead));
    Mondrian::Rect root = stage.Root();
    CHECK(std::abs(root.x + (root.width * 0.5f) - head.at.x) <= kSnap);
    CHECK(std::abs(root.y + root.height - head.at.y) <= kSnap);

    const glm::dvec3 moved = kAhead + glm::dvec3{3.0, 0.0, 0.0};
    stage.MoveEntity(moved);
    stage.Frame();
    const App::ProjectedPoint movedHead = stage.Projected(moved + glm::dvec3(kOverHead));
    root = stage.Root();
    CHECK(movedHead.at.x > head.at.x);
    CHECK(std::abs(root.x + (root.width * 0.5f) - movedHead.at.x) <= kSnap);
}

TEST_CASE("WorldWidgets: a nameplate's bound health bar reads its own entity")
{
    Stage stage;
    stage.Frame();
    stage.Frame(); // the binding reads on the sync after the screen is made
    const float full = stage.Root().width;
    REQUIRE(full > 0.f);

    stage.world.scene.GetMut<TestHealth>(stage.entity)->current = 30.f;
    stage.Frame();
    CHECK(stage.Root().width == doctest::Approx(full * 0.5f).epsilon(0.05));
}

TEST_CASE("WorldWidgets: behind the camera a widget hides, or stays on the edge nearest its entity")
{
    Stage stage;
    stage.Frame();
    stage.MoveEntity(glm::dvec3{3.0, 0.0, 10.0});
    stage.Frame();
    REQUIRE(stage.Screen() != nullptr);
    CHECK_FALSE(stage.Screen()->IsShown());

    stage.Widget().offScreen = App::WorldWidgetOffScreen::StaysOnEdge;
    stage.Frame();
    CHECK(stage.Screen()->IsShown());
    const Mondrian::Rect root = stage.Root();
    CHECK(root.x + root.width == doctest::Approx(static_cast<float>(kViewport.width)));
    CHECK(root.y >= 0.f);
}

TEST_CASE("WorldWidgets: a widget fades between its fade distances and shrinks past its scale distance")
{
    Stage stage;
    stage.Widget().fadeNear = 5.f;
    stage.Widget().fadeFar = 15.f;
    stage.Frame();
    REQUIRE(stage.Screen() != nullptr);
    // About ten metres away: half way.
    CHECK(stage.Screen()->Opacity() == doctest::Approx(0.5f).epsilon(0.05));

    stage.MoveEntity(glm::dvec3{0.0, 0.0, -20.0});
    stage.Frame();
    CHECK_FALSE(stage.Screen()->IsShown());

    stage.Widget().fadeFar = 0.f;
    stage.Widget().scaleDistance = 5.f;
    stage.Frame();
    CHECK(stage.Screen()->IsShown());
    CHECK(stage.Screen()->Opacity() == doctest::Approx(1.f));
    CHECK(stage.Screen()->Scale() == doctest::Approx(0.25f).epsilon(0.05));
}

TEST_CASE("WorldWidgets: what stands in the way shows or hides a widget, as each one asks")
{
    Stage stage;
    // A wall half way between the camera and the entity.
    const ECS::Entity wall = stage.world.scene.Create();
    stage.world.scene.Add<ECS::Transform>(wall)->position = glm::dvec3{0.0, 0.0, -5.0};
    Physics::Collider collider{};
    collider.halfExtents = glm::vec3{4.f, 4.f, 0.5f};
    (void)stage.world.scene.Add(wall, collider);
    stage.world.physics.Update(kStep);

    stage.Frame();
    REQUIRE(stage.Screen() != nullptr);
    CHECK(stage.Screen()->IsShown()); // Ignored: shown whatever is in the way

    stage.Widget().blocked = App::WorldWidgetBlocked::Hides;
    stage.Frame();
    CHECK_FALSE(stage.Screen()->IsShown());

    stage.Widget().blocked = App::WorldWidgetBlocked::Shows;
    stage.Frame();
    CHECK(stage.Screen()->IsShown());

    // With the wall gone, the other way round.
    stage.world.scene.Destroy(wall);
    stage.world.scene.FlushDestroyed();
    stage.world.physics.Update(kStep);
    stage.Frame();
    CHECK_FALSE(stage.Screen()->IsShown());
    stage.Widget().blocked = App::WorldWidgetBlocked::Hides;
    stage.Frame();
    CHECK(stage.Screen()->IsShown());
}

TEST_CASE("WorldWidgets: neither the widget's own entity nor whatever holds the camera counts as in the way")
{
    Stage stage;
    stage.Widget().blocked = App::WorldWidgetBlocked::Hides;

    SUBCASE("something attached to the entity has a collider")
    {
        // The ray ends at a point inside the box of a part hung on the entity,
        // as a character's limb or helmet would be.
        const ECS::Entity part = stage.world.scene.Create();
        (void)stage.world.scene.Add<ECS::Transform>(part);
        (void)stage.world.scene.Add(part, ECS::Parent{.parent = stage.entity});
        Physics::Collider collider{};
        collider.halfExtents = glm::vec3{1.f, 2.f, 1.f};
        (void)stage.world.scene.Add(part, collider);
    }

    SUBCASE("the camera stands inside a body that holds it")
    {
        // A first-person camera inside its character's capsule.
        const ECS::Entity body = stage.world.scene.Create();
        (void)stage.world.scene.Add<ECS::Transform>(body);
        Physics::Collider collider{};
        collider.halfExtents = glm::vec3{0.5f, 1.f, 0.5f};
        (void)stage.world.scene.Add(body, collider);
        (void)stage.world.scene.Add(stage.camera, ECS::Parent{.parent = body});
    }

    stage.world.physics.Update(kStep);
    stage.Frame();
    REQUIRE(stage.Screen() != nullptr);
    CHECK(stage.Screen()->IsShown());
}

TEST_CASE("WorldWidgets: the component reaches every player with its entity")
{
    // A character the host spawns while the game runs reaches a client only with
    // its replicated components, so a nameplate on it needs this to show there.
    const Core::Reflect::ComponentMeta *const meta = Core::Reflect::ComponentRegistry::Instance().Find("WorldWidget");
    REQUIRE(meta != nullptr);
    CHECK(meta->replicable);
    // The screen it made is each machine's own.
    const Core::Reflect::ComponentMeta *const held =
        Core::Reflect::ComponentRegistry::Instance().Find("WorldWidgetScreen");
    REQUIRE(held != nullptr);
    CHECK_FALSE(held->replicable);
    CHECK_FALSE(held->serializable);
}

TEST_CASE("WorldWidgets: a widget goes with its entity, and with its component")
{
    SUBCASE("the entity destroyed")
    {
        Stage stage;
        stage.Frame();
        REQUIRE(stage.world.screenStack.size() == 1);
        stage.world.scene.Destroy(stage.entity);
        stage.world.scene.FlushDestroyed();
        // Gone before the next frame draws it.
        stage.Frame();
        CHECK(stage.world.screenStack.empty());
    }

    SUBCASE("the entity destroyed before its screen was ever drawn")
    {
        // The screen is made by the system and the entity goes before the UI
        // has looked at it once: its owner still takes it away.
        Stage stage;
        App::SystemContext ctx{stage.world, kStep, 0, nullptr, nullptr, stage.universal, true, nullptr};
        ctx.ui = &stage.ui;
        stage.ui.ProcessInput({});
        App::WorldWidgetSystem(ctx);
        REQUIRE(stage.world.screenStack.size() == 1);
        stage.world.scene.Destroy(stage.entity);
        stage.world.scene.FlushDestroyed();
        stage.ui.Sync(kViewport);
        stage.Frame();
        CHECK(stage.world.screenStack.empty());
    }

    SUBCASE("the component taken off")
    {
        Stage stage;
        stage.Frame();
        REQUIRE(stage.world.screenStack.size() == 1);
        (void)stage.world.scene.Remove<App::WorldWidget>(stage.entity);
        stage.Frame();
        CHECK(stage.world.screenStack.empty());
        CHECK_FALSE(stage.world.scene.Has<App::WorldWidgetScreen>(stage.entity));
    }
}
