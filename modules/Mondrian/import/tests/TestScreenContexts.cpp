/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using namespace ScreenTesting;

namespace
{

/// What @p context does at @p moment.
const ContextAction &ActionAt(const ScreenContext &context, ContextMoment moment)
{
    return context.actions[static_cast<std::size_t>(moment)];
}

/// The message refusing a screen holding only @p context, which the case
/// expects to be refused.
std::string RefusedContext(std::string_view context)
{
    return Refused("<screen>\n    " + std::string{context} + "\n</screen>\n").message;
}

bool Says(const std::string &message, std::string_view part)
{
    return message.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("ScreenContexts: a context says what it does at each moment, with an event or a verb")
{
    const ScreenDocument document = Compiled(R"amdn(<screen>
    <context name="boss" on_set="Game::QuitRequested" on_lost="hide()" />
    <context on_switch="hide()" on_clear="Game::QuitRequested" />
    <text>Boss</text>
</screen>)amdn");

    REQUIRE(document.contexts.size() == 2);
    const ScreenContext &boss = document.contexts[0];
    CHECK(boss.name.View() == "boss");
    CHECK(ActionAt(boss, ContextMoment::Set).action == ActionKind::Event);
    CHECK(ActionAt(boss, ContextMoment::Set).eventName.View() == "Game::QuitRequested");
    CHECK(ActionAt(boss, ContextMoment::Switch).action == ActionKind::None);
    CHECK(ActionAt(boss, ContextMoment::Clear).action == ActionKind::None);
    CHECK(ActionAt(boss, ContextMoment::Lost).action == ActionKind::Verb);
    CHECK(ActionAt(boss, ContextMoment::Lost).verb == ScreenVerb::Hide);

    const ScreenContext &unnamed = document.contexts[1];
    CHECK(unnamed.name.Empty());
    CHECK(ActionAt(unnamed, ContextMoment::Switch).action == ActionKind::Verb);
    CHECK(ActionAt(unnamed, ContextMoment::Clear).eventName.View() == "Game::QuitRequested");

    // A context is no node: the screen holds the root and the text.
    CHECK(document.nodes.size() == 2);
}

TEST_CASE("ScreenContexts: a context that says nothing is still declared")
{
    const ScreenDocument document = Compiled(R"amdn(<screen><context name="boss" /></screen>)amdn");
    REQUIRE(document.contexts.size() == 1);
    CHECK(document.contexts[0].name.View() == "boss");
}

TEST_CASE("ScreenContexts: a moment it cannot carry out is refused where it was written")
{
    SUBCASE("an event this build does not declare")
    {
        CHECK(Says(RefusedContext(R"amdn(<context on_lost="Game::Nothing" />)amdn"), "Game::Nothing"));
    }

    SUBCASE("a verb written without parens")
    {
        CHECK(Says(RefusedContext(R"amdn(<context on_lost="hide" />)amdn"), "hide()"));
    }

    SUBCASE("a verb this build does not have")
    {
        CHECK(Says(RefusedContext(R"amdn(<context on_lost="frobnicate()" />)amdn"), "frobnicate"));
    }

    SUBCASE("a verb that acts on a control")
    {
        // A moment comes from no control, so there is nothing to step.
        CHECK(Says(RefusedContext(R"amdn(<context on_lost="step(volume, 1)" />)amdn"), "control"));
    }

    SUBCASE("a verb given what it does not take")
    {
        CHECK(Says(RefusedContext(R"amdn(<context on_lost="hide(volume)" />)amdn"), "hide()"));
    }

    SUBCASE("a refusal points where it was written")
    {
        CHECK(Refused("<screen>\n<context\n on_lost=\"hide\" />\n</screen>\n").line == 3);
    }
}

TEST_CASE("ScreenContexts: a context is declared once, by a name that can be written in a binding")
{
    SUBCASE("two of one name")
    {
        CHECK(Says(RefusedContext(R"amdn(<context name="boss" /><context name="boss" />)amdn"), "boss"));
    }

    SUBCASE("two unnamed")
    {
        CHECK(Says(RefusedContext(R"amdn(<context /><context />)amdn"), "unnamed"));
    }

    SUBCASE("a name that is not one")
    {
        CHECK(Says(RefusedContext(R"amdn(<context name="boss:bar" />)amdn"), "boss:bar"));
        CHECK(Says(RefusedContext(R"amdn(<context name="1boss" />)amdn"), "1boss"));
        CHECK(Says(RefusedContext(R"amdn(<context name="" />)amdn"), "name"));
    }

    SUBCASE("an attribute a context does not have")
    {
        const std::string message = RefusedContext(R"amdn(<context on_click="hide()" />)amdn");
        CHECK(Says(message, "on_click"));
        // The message names the ones it does.
        CHECK(Says(message, "on_lost"));
    }
}

TEST_CASE("ScreenContexts: a context is declared directly inside the screen, and holds nothing")
{
    SUBCASE("inside an element")
    {
        CHECK(Says(RefusedContext(R"amdn(<row><context name="boss" /></row>)amdn"), "directly inside"));
    }

    SUBCASE("holding an element")
    {
        CHECK(Says(RefusedContext(R"amdn(<context name="boss"><text>Boss</text></context>)amdn"), "neither"));
    }

    SUBCASE("holding text")
    {
        CHECK(Says(RefusedContext(R"amdn(<context name="boss">Boss</context>)amdn"), "neither"));
    }

    SUBCASE("as a template's name")
    {
        CHECK(Says(RefusedContext(R"amdn(<template name="context"><row /></template>)amdn"), "already an element"));
    }
}
