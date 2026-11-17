/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/FieldMeta.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using namespace ScreenTesting;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;

namespace
{

/// The one component these screens bind to.
struct Health
{
    uint64_t owner = 0;
    float current = 0.f;
    uint8_t stance = 0;
    bool alive = false;
};

FieldMeta Field(std::string name, FieldType type, std::size_t offset)
{
    FieldMeta field;
    field.name = std::move(name);
    field.type = type;
    field.offset = offset;
    return field;
}

FieldMeta StanceField()
{
    FieldMeta stance = Field("stance", FieldType::Enum, offsetof(Health, stance));
    stance.enumSize = sizeof(uint8_t);
    stance.enumConstants = {{.name = "Standing", .value = 0}, {.name = "Crouching", .value = 1}};
    return stance;
}

const std::vector<ComponentMeta> &Components()
{
    static const std::vector<ComponentMeta> components{
        ComponentMeta{.name = "Health",
                      .fields = {Field("owner", FieldType::EntityRef, offsetof(Health, owner)),
                                 Field("current", FieldType::Float, offsetof(Health, current)), StanceField(),
                                 Field("alive", FieldType::Bool, offsetof(Health, alive))},
                      .typeIndex = typeid(Health)}};
    return components;
}

const ComponentMeta *FindComponent(std::string_view name)
{
    for (const ComponentMeta &component : Components())
    {
        if (component.name == name)
        {
            return &component;
        }
    }
    return nullptr;
}

/// Rules that know Health, and require keys when @p literals says so.
ScreenRules WithHealth(LiteralText literals = LiteralText::Allowed)
{
    ScreenRules rules;
    rules.literals = literals;
    rules.components = FindComponent;
    rules.tables.byName["hud"].entries = {{"stance.Standing", "Up"},
                                          {"stance.Crouching", "Down"},
                                          {"flag.true", "Yes"},
                                          {"flag.false", "No"},
                                          {"half.Standing", "Up"},
                                          {"half.true", "Yes"}};
    return rules;
}

ScreenDocument Bound(std::string_view text)
{
    return Compiled(text, {}, WithHealth());
}

/// The message refusing a screen holding @p inside.
std::string RefusedInside(std::string_view inside, LiteralText literals = LiteralText::Allowed)
{
    return Refused("<screen>\n" + std::string{inside} + "\n</screen>\n", {}, WithHealth(literals)).message;
}

bool Says(const std::string &message, std::string_view part)
{
    return message.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("ScreenBindings: bound text is kept as written, for the loader to read")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <text name="hp">HP {Health.current:.0f} {{max}}</text>
</screen>)amdn");
    const ScreenNode &hp = NodeNamed(document, "hp");
    CHECK(hp.textIsBound);
    CHECK_FALSE(hp.textIsKey);
    CHECK(document.Text(hp.text) == "HP {Health.current:.0f} {{max}}");
}

TEST_CASE("ScreenBindings: text with doubled braces and no binding is shown as written, one brace each")
{
    const ScreenDocument document = Bound(R"amdn(<screen><text name="t">{{not bound}}</text></screen>)amdn");
    const ScreenNode &text = NodeNamed(document, "t");
    CHECK_FALSE(text.textIsBound);
    CHECK(document.Text(text.text) == "{not bound}");
}

TEST_CASE("ScreenBindings: a button's label can be bound")
{
    const ScreenDocument document =
        Bound(R"amdn(<screen><button name="b" on_click="hide()">{Health.current}</button></screen>)amdn");
    CHECK(NodeNamed(document, "b").textIsBound);
}

TEST_CASE("ScreenBindings: visible bound to a bool keeps the binding beside the node")
{
    const ScreenDocument document =
        Bound(R"amdn(<screen><text name="t" visible="{Health.alive}">Alive</text></screen>)amdn");
    const ScreenNode &text = NodeNamed(document, "t");
    CHECK(document.Text(text.visibleBinding) == "{Health.alive}");
    CHECK(text.visible); // what the loader hides until the binding reads
}

TEST_CASE("ScreenBindings: a binding naming a context reads one the screen declares, above or below it")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <text name="t">{boss:Health.current}</text>
    <context name="boss" />
</screen>)amdn");
    CHECK(NodeNamed(document, "t").textIsBound);
}

TEST_CASE("ScreenBindings: a binding the cook cannot resolve is refused where it was written")
{
    SUBCASE("a component no reflected type has")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>{Mana.current}</text>)amdn"), "'Mana'"));
    }

    SUBCASE("a field its component does not have")
    {
        const std::string message = RefusedInside(R"amdn(<text>{Health.curent}</text>)amdn");
        CHECK(Says(message, "'curent'"));
        CHECK(Says(message, "Health"));
    }

    SUBCASE("a field text cannot show")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>{Health.owner}</text>)amdn"), "cannot show"));
    }

    SUBCASE("a format the field's type does not take")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>{Health.alive:.1f}</text>)amdn"), "format"));
    }

    SUBCASE("a context the screen does not declare")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>{boss:Health.current}</text>)amdn"), "<context name=\"boss\" />"));
    }

    SUBCASE("a brace that closes nothing")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>HP}</text>)amdn"), "}}"));
    }

    SUBCASE("on the line it was written")
    {
        CHECK(Refused("<screen>\n<text>\n{Health.curent}</text>\n</screen>\n", {}, WithHealth()).line == 2);
    }
}

TEST_CASE("ScreenBindings: only a text's words and a button's label are filled from the world")
{
    SUBCASE("a text field's text is the player's")
    {
        CHECK(Says(RefusedInside(R"amdn(<text_field>{Health.current}</text_field>)amdn"), "binding"));
    }

    SUBCASE("a placeholder")
    {
        CHECK(Says(RefusedInside(R"amdn(<text_field placeholder="{Health.current}" />)amdn"), "binding"));
    }
}

TEST_CASE("ScreenBindings: visible takes one bool binding and nothing else")
{
    CHECK(Says(RefusedInside(R"amdn(<text visible="{Health.current}">x</text>)amdn"), "not a bool"));
    CHECK(Says(RefusedInside(R"amdn(<text visible="is {Health.alive}">x</text>)amdn"), "one binding"));
}

TEST_CASE("ScreenBindings: where text must come from a table, only the words around a binding are refused")
{
    // A number has nothing to translate; the words around it do.
    const ScreenDocument document =
        Compiled(R"amdn(<screen><text name="t">{Health.current}</text></screen>)amdn", {},
                 WithHealth(LiteralText::RequiresKey));
    CHECK(NodeNamed(document, "t").textIsBound);

    CHECK(Says(RefusedInside(R"amdn(<text>HP {Health.current}</text>)amdn", LiteralText::RequiresKey),
               "string table"));
}

TEST_CASE("ScreenBindings: a key on an enum or a bool is kept as written, for the loader to read")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <text name="t">{Health.stance:#hud:stance} {Health.alive:#hud:flag}</text>
</screen>)amdn");
    const ScreenNode &text = NodeNamed(document, "t");
    CHECK(text.textIsBound);
    CHECK(document.Text(text.text) == "{Health.stance:#hud:stance} {Health.alive:#hud:flag}");
}

TEST_CASE("ScreenBindings: a key is refused unless its table labels every value")
{
    SUBCASE("an enumerator with no entry")
    {
        const std::string message = RefusedInside(R"amdn(<text>{Health.stance:#hud:half}</text>)amdn");
        CHECK(Says(message, "half.Crouching"));
        CHECK(Says(message, "'hud'"));
    }

    SUBCASE("a bool's other value with no entry")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>{Health.alive:#hud:half}</text>)amdn"), "half.false"));
    }

    SUBCASE("a table the project does not list")
    {
        const std::string message = RefusedInside(R"amdn(<text>{Health.stance:#nowhere:stance}</text>)amdn");
        CHECK(Says(message, "'nowhere'"));
        CHECK(Says(message, "config/ui.json"));
    }

    SUBCASE("a key naming no table")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>{Health.stance:#stance}</text>)amdn"), "names no table"));
    }

    SUBCASE("a key on a field that is neither an enum nor a bool")
    {
        CHECK(Says(RefusedInside(R"amdn(<text>{Health.current:#hud:stance}</text>)amdn"), "format"));
    }
}

TEST_CASE("ScreenBindings: where text must come from a table, an enum or a bool needs a key")
{
    CHECK(Says(RefusedInside(R"amdn(<text>{Health.stance}</text>)amdn", LiteralText::RequiresKey), "from the code"));
    CHECK(Says(RefusedInside(R"amdn(<text>{Health.alive}</text>)amdn", LiteralText::RequiresKey), "from the code"));
    CHECK(Says(RefusedInside(R"amdn(<button>{Health.alive}</button>)amdn", LiteralText::RequiresKey), "from the code"));

    const ScreenRules rules = WithHealth(LiteralText::RequiresKey);
    CHECK(NodeNamed(Compiled(R"amdn(<screen><text name="t">{Health.stance:#hud:stance}</text></screen>)amdn", {}, rules),
                    "t")
              .textIsBound);

    // A bool deciding what shows puts no word of its own on screen.
    const ScreenDocument visible =
        Compiled(R"amdn(<screen><text name="t" visible="{Health.alive}">#hud:flag.true</text></screen>)amdn", {}, rules);
    CHECK(visible.Text(NodeNamed(visible, "t").visibleBinding) == "{Health.alive}");

    // A screen players never see is exempt, as its other text is.
    CHECK(NodeNamed(Compiled(R"amdn(<screen debug_only="true"><text name="t">{Health.alive}</text></screen>)amdn", {},
                             rules),
                    "t")
              .textIsBound);
}

TEST_CASE("ScreenBindings: a template checked alone may name a context the screen using it declares")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <template name="boss_bar">
        <text>{boss:Health.current}</text>
    </template>
    <context name="boss" />
    <boss_bar name="bar" />
</screen>)amdn");
    CHECK(NodeNamed(document, "bar").textIsBound);

    // Used where nothing declares it, the instance is refused.
    CHECK(Says(RefusedInside(R"amdn(<template name="boss_bar"><text>{boss:Health.current}</text></template>
<boss_bar />)amdn"),
               "does not declare"));
}
