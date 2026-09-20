/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/ScreenLoader.hpp>

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/EventQueue.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian;
using Assisi::Core::EventCatalog;
using Assisi::Core::EventQueue;

namespace
{

constexpr Extent kViewport{1280, 720};

/// A button big enough to aim at. These tests set no font, so text takes no
/// space and a button left to fit its label would be nothing to click.
constexpr float kButtonWidth = 200.f;
constexpr float kButtonHeight = 60.f;

/// The event a document's Quit button names, and a second nobody names — so a
/// test can tell "pushed the right one" from "pushed something".
struct QuitRequested
{
};

struct NeverWanted
{
};

EventCatalog TwoEvents()
{
    EventCatalog catalog;
    catalog.Register({.name = "Game::QuitRequested", .push = [](EventQueue &events) { events.Push(QuitRequested{}); }});
    catalog.Register({.name = "Game::NeverWanted", .push = [](EventQueue &events) { events.Push(NeverWanted{}); }});
    return catalog;
}

/// The pause menu's shape: a root that blocks the pointer, a panel, a title, a
/// Resume that hides and a Quit that announces.
ScreenDocument PauseMenu()
{
    ScreenDocument document;
    document.name = "Pause";
    document.sortKey = kSortMenu;
    document.traits = {
        .input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::HidesBeneath, .pause = ScreenPause::Pause};
    document.systems = {"PauseMenu"};

    ScreenNode root;
    root.blocksPointer = true;
    root.style.childAlign = {Alignment::Center, Alignment::Center};
    document.nodes.push_back(root);

    ScreenNode panel;
    panel.parent = 0;
    panel.name = "panel";
    panel.style.direction = Direction::Column;
    panel.style.gap = 20.f;
    document.nodes.push_back(panel);

    ScreenNode title;
    title.parent = 1;
    title.name = "title";
    title.text = "Paused";
    title.style.textSize = 48.f;
    document.nodes.push_back(title);

    ScreenNode resume;
    resume.parent = 1;
    resume.name = "resume";
    resume.text = "Resume";
    resume.widget = BuiltinWidget::Button;
    resume.action = ActionKind::Verb;
    resume.verb = ScreenVerb::Hide;
    resume.style.sizing = {Sizing::Fixed(kButtonWidth), Sizing::Fixed(kButtonHeight)};
    document.nodes.push_back(resume);

    ScreenNode quit;
    quit.parent = 1;
    quit.name = "quit";
    quit.text = "Quit";
    quit.widget = BuiltinWidget::Button;
    quit.action = ActionKind::Event;
    quit.eventName = "Game::QuitRequested";
    quit.style.sizing = {Sizing::Fixed(kButtonWidth), Sizing::Fixed(kButtonHeight)};
    document.nodes.push_back(quit);

    document.focus = 3; // Resume
    return document;
}

/// The centre of the node called @p name, as the last frame placed it.
Point CentreOf(const Screen &screen, std::string_view name)
{
    const LayoutNode *node = screen.GetLayout().Get(screen.Find(name));
    REQUIRE(node != nullptr);
    return {.x = node->rect.x + (node->rect.width / 2.f), .y = node->rect.y + (node->rect.height / 2.f)};
}

/// Clicks the node called @p name: press and release on the same node, which is
/// what the UI turns into an activation.
void Click(Ui &ui, const Screen &screen, std::string_view name)
{
    const Point centre = CentreOf(screen, name);

    UiInput press;
    press.pointer = centre;
    press.grant = InputGrant::Everything;
    press.primaryDown = true;
    press.primaryPressed = true;
    ui.ProcessInput(press);
    ui.Sync(kViewport);

    UiInput release;
    release.pointer = centre;
    release.grant = InputGrant::Everything;
    release.primaryReleased = true;
    ui.ProcessInput(release);
    ui.Sync(kViewport);
}

} // namespace

TEST_CASE("ScreenLoader: a document becomes the tree it describes")
{
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, PauseMenu(), catalog);
    REQUIRE(loaded.has_value());

    Screen &screen = *loaded->screen;
    CHECK(screen.Name() == "Pause");
    CHECK(screen.SortKey() == kSortMenu);
    CHECK(screen.Traits().input == ScreenInput::ConsumeInput);
    CHECK(screen.Traits().beneath == ScreenBeneath::HidesBeneath);
    CHECK(screen.Traits().pause == ScreenPause::Pause);

    // The systems come back beside the screen rather than installed: the UI has
    // no route to a world and does not try to reach one.
    REQUIRE(loaded->systems.size() == 1);
    CHECK(loaded->systems[0] == "PauseMenu");

    const NodeId title = screen.Find("title");
    REQUIRE(screen.Tree().IsAlive(title));
    CHECK(screen.Tree().Get(title)->text == "Paused");
    CHECK(screen.Tree().Get(title)->style.textSize == 48.f);

    // The parent link the flat table describes, rebuilt.
    const NodeId panel = screen.Find("panel");
    CHECK(screen.Tree().Get(title)->parent == panel);
    CHECK(screen.Tree().Get(panel)->parent == screen.Root());

    // The root is the tree's own, styled from node zero rather than created.
    CHECK(screen.Tree().Get(screen.Root())->blocksPointer);
    CHECK(screen.Tree().Get(screen.Root())->style.childAlign[0] == Alignment::Center);
}

TEST_CASE("ScreenLoader: a verb button acts on its own screen")
{
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, PauseMenu(), catalog);
    REQUIRE(loaded.has_value());

    Screen &screen = *loaded->screen;
    screen.Show();
    ui.ProcessInput({});
    ui.Sync(kViewport);
    REQUIRE(screen.IsShown());

    Click(ui, screen, "resume");

    // Hide is carried on the node, so it needs no event, no system and nothing
    // named in a level.
    CHECK_FALSE(screen.IsShown());
}

TEST_CASE("ScreenLoader: an event button pushes the event it names and no other")
{
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, PauseMenu(), catalog);
    REQUIRE(loaded.has_value());

    Screen &screen = *loaded->screen;
    screen.Show();
    ui.ProcessInput({});
    ui.Sync(kViewport);

    Click(ui, screen, "quit");

    CHECK(events.Read<QuitRequested>().size() == 1);
    CHECK(events.Read<NeverWanted>().empty());
}

TEST_CASE("ScreenLoader: the document's focus is what has the keys")
{
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, PauseMenu(), catalog);
    REQUIRE(loaded.has_value());

    Screen &screen = *loaded->screen;
    screen.Show();
    ui.ProcessInput({});
    ui.Sync(kViewport);

    // Focus starts on Resume, so a player reaching for the keyboard is one
    // press from carrying on rather than one press from leaving.
    CHECK(ui.GetInteraction().focused == screen.Find("resume"));
}

TEST_CASE("ScreenLoader: an unknown event leaves no screen behind")
{
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    ScreenDocument document = PauseMenu();
    document.nodes[4].eventName = "Game::Misspelt";

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, document, catalog);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ScreenLoadError::UnknownEvent);

    // A screen joins its Ui in its own constructor, so one built and abandoned
    // half way would still be drawn. Nothing was built, so nothing takes input.
    CHECK(ui.InputScreen() == nullptr);
    CHECK_FALSE(ui.TakesInput());
}

TEST_CASE("ScreenLoader: a named style is carried and ignored")
{
    // There is nowhere for a style name to resolve to yet. A file written today
    // should load, and start resolving when there is.
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    ScreenDocument document = PauseMenu();
    document.nodes[1].styleName = "Panel";

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, document, catalog);
    REQUIRE(loaded.has_value());
    // The node's own attributes are what it looks like, untouched by the name.
    CHECK(loaded->screen->Tree().Get(loaded->screen->Find("panel"))->style.gap == 20.f);
}

TEST_CASE("ScreenLoader: a document that is not a walkable tree is refused")
{
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    SUBCASE("no root")
    {
        const ScreenDocument empty;
        const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, empty, catalog);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ScreenLoadError::BadDocument);
    }

    SUBCASE("a parent that points forward")
    {
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes.emplace_back();
        document.nodes[1].parent = 1;

        const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, document, catalog);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ScreenLoadError::BadDocument);
    }

    CHECK(ui.InputScreen() == nullptr);
}

TEST_CASE("ScreenLoader: a control or an action on the root is refused")
{
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    SUBCASE("a control")
    {
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].widget = BuiltinWidget::Button;

        const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, document, catalog);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ScreenLoadError::MisplacedNode);
    }

    SUBCASE("an action")
    {
        // The root is the screen itself; an action there would never fire.
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].action = ActionKind::Verb;

        const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, document, catalog);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.error() == ScreenLoadError::MisplacedNode);
    }
}

TEST_CASE("ScreenLoader: a control this build does not build is named as such")
{
    // The element table and the loader's switch grow together. A slider that
    // silently became a plain box would be a screen that looks nearly right.
    EventQueue events;
    Ui ui{events};
    const EventCatalog catalog = TwoEvents();

    ScreenDocument document;
    document.nodes.emplace_back();
    ScreenNode slider;
    slider.parent = 0;
    slider.widget = BuiltinWidget::ContinuousSlider;
    document.nodes.push_back(slider);

    const std::expected<LoadedScreen, ScreenLoadError> loaded = InstantiateScreen(ui, document, catalog);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ScreenLoadError::UnsupportedWidget);
}
