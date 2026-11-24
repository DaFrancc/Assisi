/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/FieldMeta.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Math/Color.hpp>

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

/// A component with a field for each attribute a binding drives.
struct Banner
{
    Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> colour{0.f, 0.f, 0.f, 0.f};
    Assisi::Core::AssetId portrait;
    Assisi::Core::ShortString icon;
    float fill = 0.f;
};

const std::vector<ComponentMeta> &Components()
{
    static const std::vector<ComponentMeta> components{
        ComponentMeta{.name = "Health",
                      .fields = {Field("owner", FieldType::EntityRef, offsetof(Health, owner)),
                                 Field("current", FieldType::Float, offsetof(Health, current)), StanceField(),
                                 Field("alive", FieldType::Bool, offsetof(Health, alive))},
                      .typeIndex = typeid(Health)},
        ComponentMeta{.name = "Banner",
                      .fields = {Field("colour", FieldType::SrgbColor4, offsetof(Banner, colour)),
                                 Field("portrait", FieldType::AssetId, offsetof(Banner, portrait)),
                                 Field("icon", FieldType::String, offsetof(Banner, icon)),
                                 Field("fill", FieldType::Float, offsetof(Banner, fill))},
                      .typeIndex = typeid(Banner)}};
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

namespace
{

/// What @p node keeps for @p attribute, or null.
const AttributeBinding *Kept(const ScreenNode &node, BoundAttribute attribute)
{
    for (const AttributeBinding &bound : node.boundAttributes)
    {
        if (bound.attribute == attribute)
        {
            return &bound;
        }
    }
    return nullptr;
}

/// The binding @p node keeps for @p attribute, or empty.
std::string_view BindingOf(const ScreenDocument &document, const ScreenNode &node, BoundAttribute attribute)
{
    const AttributeBinding *const bound = Kept(node, attribute);
    return bound != nullptr ? document.Text(bound->binding) : std::string_view{};
}

} // namespace

TEST_CASE("ScreenBindings: a colour, a picture and a size can each follow a field")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <column name="c" background="{Banner.colour}" tint="{Banner.colour}" width="{Banner.fill}% max 400" />
    <text name="x" border_color="{Banner.colour}" text_color="{Banner.colour}" height="{Banner.fill}">x</text>
    <image name="i" sprite="{Banner.icon}" />
    <image name="t" texture="{Banner.portrait}" background_image="{Banner.icon}" />
</screen>)amdn");
    const ScreenNode &column = NodeNamed(document, "c");
    CHECK(BindingOf(document, column, BoundAttribute::Background) == "{Banner.colour}");
    CHECK(BindingOf(document, column, BoundAttribute::Tint) == "{Banner.colour}");
    CHECK(BindingOf(document, column, BoundAttribute::Width) == "{Banner.fill}");
    // Not written until the binding reads, so a style it names shows until then.
    CHECK((column.written & PropertyBit(StyleProperty::Background)) == 0);
    CHECK((column.written & PropertyBit(StyleProperty::Width)) == 0);
    // The unit and the limits wait beside the binding; the style keeps its own.
    const Sizing &width = Kept(column, BoundAttribute::Width)->size;
    CHECK(width.kind == SizingKind::Fixed);
    CHECK(width.value == Percent(0.f));
    CHECK(width.max == Px(400.f));
    CHECK(column.style.sizing[static_cast<std::size_t>(Axis::X)] == Sizing::Fit());

    const ScreenNode &text = NodeNamed(document, "x");
    CHECK(BindingOf(document, text, BoundAttribute::BorderColor) == "{Banner.colour}");
    CHECK(BindingOf(document, text, BoundAttribute::TextColor) == "{Banner.colour}");
    CHECK(BindingOf(document, text, BoundAttribute::Height) == "{Banner.fill}");
    CHECK(Kept(text, BoundAttribute::Height)->size.value == Px(0.f));

    CHECK(BindingOf(document, NodeNamed(document, "i"), BoundAttribute::Sprite) == "{Banner.icon}");
    const ScreenNode &texture = NodeNamed(document, "t");
    CHECK(BindingOf(document, texture, BoundAttribute::Texture) == "{Banner.portrait}");
    CHECK(BindingOf(document, texture, BoundAttribute::BackgroundImage) == "{Banner.icon}");
}

TEST_CASE("ScreenBindings: an attribute bound to a field of the wrong type fails the cook")
{
    CHECK(Says(RefusedInside(R"amdn(<column background="{Banner.fill}" />)amdn"), "attribute cannot take"));
    CHECK(Says(RefusedInside(R"amdn(<column width="{Banner.colour}" />)amdn"), "attribute cannot take"));
    CHECK(Says(RefusedInside(R"amdn(<image texture="{Banner.icon}" />)amdn"), "attribute cannot take"));
    CHECK(Says(RefusedInside(R"amdn(<image sprite="{Banner.portrait}" />)amdn"), "attribute cannot take"));
    CHECK(Says(RefusedInside(R"amdn(<column background="{Banner.colur}" />)amdn"), "'colur'"));
}

TEST_CASE("ScreenBindings: an attribute's binding is the whole value, with no format and no state")
{
    CHECK(Says(RefusedInside(R"amdn(<column width="{Banner.fill:.1f}" />)amdn"), "format"));
    CHECK(Says(RefusedInside(R"amdn(<column background="a {Banner.colour}" />)amdn"), "one binding"));
    CHECK(Says(RefusedInside(R"amdn(<button hover:background="{Banner.colour}">x</button>)amdn"), "state"));
    CHECK(Says(RefusedInside(R"amdn(<column width="{Banner.fill}px" />)amdn"), "after its binding"));
    CHECK(Says(RefusedInside(R"amdn(<column width="grow max {Banner.fill}" />)amdn"), "comes first"));
}

TEST_CASE("ScreenBindings: a node bound to one picture names no other")
{
    CHECK(Says(RefusedInside(R"amdn(<image sprite="{Banner.icon}" texture="{Banner.portrait}" />)amdn"),
               "one picture"));
}

TEST_CASE("ScreenBindings: what an instance writes for an attribute wins over its template's, bound or not")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <template name="bound"><column background="{Banner.colour}" width="{Banner.fill}" /></template>
    <template name="plain"><column background="#ffffff" width="20" /></template>
    <bound name="b" background="#000000" />
    <plain name="p" width="{Banner.fill}%" />
</screen>)amdn");
    const ScreenNode &bound = NodeNamed(document, "b");
    CHECK(BindingOf(document, bound, BoundAttribute::Background).empty());
    CHECK((bound.written & PropertyBit(StyleProperty::Background)) != 0);
    CHECK(BindingOf(document, bound, BoundAttribute::Width) == "{Banner.fill}");

    const ScreenNode &plain = NodeNamed(document, "p");
    CHECK(BindingOf(document, plain, BoundAttribute::Width) == "{Banner.fill}");
    CHECK((plain.written & PropertyBit(StyleProperty::Width)) == 0);
    CHECK(Kept(plain, BoundAttribute::Width)->size.value == Percent(0.f));
    CHECK((plain.written & PropertyBit(StyleProperty::Background)) != 0);
}
