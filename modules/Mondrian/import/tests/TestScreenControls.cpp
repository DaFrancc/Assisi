/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/Reflect/ComponentMeta.hpp>
#include <Assisi/Core/Reflect/FieldMeta.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

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
using Assisi::Core::DefineEvent;
using Assisi::Core::EventCatalog;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Core::Reflect::StructSpec;

namespace
{

/// The component these screens bind to.
struct Health
{
    float current = 0.f;
    int32_t lives = 0;
    uint8_t stance = 0;
    bool alive = false;
};

/// What a registered source of these screens would be.
struct OptionsView
{
    float volume = 0.f;
    int32_t quality = 0;
    bool vsync = false;
};

FieldMeta Field(std::string name, FieldType type, std::size_t offset)
{
    FieldMeta field;
    field.name = std::move(name);
    field.type = type;
    field.offset = offset;
    return field;
}

const ComponentMeta &HealthMeta()
{
    static const ComponentMeta meta = []
    {
        FieldMeta stance = Field("stance", FieldType::Enum, offsetof(Health, stance));
        stance.enumSize = sizeof(uint8_t);
        stance.enumConstants = {{.name = "Standing", .value = 0}, {.name = "Crouching", .value = 1}};
        return ComponentMeta{.name = "Health",
                             .fields = {Field("current", FieldType::Float, offsetof(Health, current)),
                                        Field("lives", FieldType::Int32, offsetof(Health, lives)), stance,
                                        Field("alive", FieldType::Bool, offsetof(Health, alive))},
                             .typeIndex = typeid(Health)};
    }();
    return meta;
}

const StructSpec &OptionsSpec()
{
    static const StructSpec spec{.name = "OptionsView",
                                 .fields = {Field("volume", FieldType::Float, offsetof(OptionsView, volume)),
                                            Field("quality", FieldType::Int32, offsetof(OptionsView, quality)),
                                            Field("vsync", FieldType::Bool, offsetof(OptionsView, vsync))}};
    return spec;
}

const StructSpec &ClashingSpec()
{
    static const StructSpec spec{.name = "Health",
                                 .fields = {Field("current", FieldType::Float, offsetof(Health, current))}};
    return spec;
}

ScreenRules Rules(bool clashing = false)
{
    ScreenRules rules;
    rules.components = [](std::string_view name) -> const ComponentMeta *
    { return name == "Health" ? &HealthMeta() : nullptr; };
    rules.structs = [clashing](std::string_view name) -> const StructSpec *
    {
        if (name == "OptionsView")
        {
            return &OptionsSpec();
        }
        return clashing && name == "Health" ? &ClashingSpec() : nullptr;
    };
    return rules;
}

struct Saved
{
};

struct Moved
{
    float value = 0.f;
};

struct Flipped
{
    bool value = false;
};

struct Picked
{
    int32_t value = 0;
};

struct Named
{
    std::string value;
};

/// One event of each kind a `value` field can be.
EventCatalog ValueEvents()
{
    EventCatalog catalog;
    catalog.Register(DefineEvent<Saved>("Game::Saved"));
    catalog.Register(DefineEvent<Moved>("Game::Moved"));
    catalog.Register(DefineEvent<Flipped>("Game::Flipped"));
    catalog.Register(DefineEvent<Picked>("Game::Picked"));
    catalog.Register(DefineEvent<Named>("Game::Named"));
    return catalog;
}

/// @p text compiled, which the case expects to work.
ScreenDocument Bound(std::string_view text, const Files &files = {})
{
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));
    std::expected<ScreenDocument, MarkupError> document = CompileScreen(*parsed, ValueEvents(), Serving(files), Rules());
    REQUIRE_MESSAGE(document.has_value(), Why(document));
    return *std::move(document);
}

/// The message refusing a screen holding @p inside.
std::string RefusedInside(std::string_view inside, bool clashing = false)
{
    const std::string text = "<screen>\n" + std::string{inside} + "\n</screen>\n";
    const std::expected<MarkupElement, MarkupError> parsed = ParseMarkup(text);
    REQUIRE_MESSAGE(parsed.has_value(), Why(parsed));
    const std::expected<ScreenDocument, MarkupError> document =
        CompileScreen(*parsed, ValueEvents(), NoFiles(), Rules(clashing));
    REQUIRE_FALSE(document.has_value());
    return document.error().message;
}

bool Says(const std::string &message, std::string_view part)
{
    return message.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("ScreenControls: a control's binding is kept as written, for the loader to read")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <toggle name="sync" on="{=OptionsView.vsync}" />
    <slider name="volume" value="{=OptionsView.volume}" min="0" max="1" step="0.05" />
    <stepped_slider name="quality" value="{=OptionsView.quality}" steps="4" />
    <slider name="health" value="{Health.current}" min="0" max="100" step="1" />
    <stepped_slider name="stance" value="{=Health.stance}" />
    <slider name="plain" value="30" min="0" max="100" step="1" />
</screen>)amdn");
    CHECK(document.Text(NodeNamed(document, "sync").valueBinding) == "{=OptionsView.vsync}");
    CHECK(document.Text(NodeNamed(document, "volume").valueBinding) == "{=OptionsView.volume}");
    CHECK(document.Text(NodeNamed(document, "quality").valueBinding) == "{=OptionsView.quality}");
    CHECK(document.Text(NodeNamed(document, "health").valueBinding) == "{Health.current}");
    CHECK(document.Text(NodeNamed(document, "stance").valueBinding) == "{=Health.stance}");

    const ScreenNode &plain = NodeNamed(document, "plain");
    CHECK(document.Text(plain.valueBinding).empty());
    CHECK(plain.value == doctest::Approx(30.f));
}

TEST_CASE("ScreenControls: a source reads one-way wherever a component does")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <text name="t" visible="{OptionsView.vsync}">Volume {OptionsView.volume:.1f}</text>
</screen>)amdn");
    const ScreenNode &text = NodeNamed(document, "t");
    CHECK(text.textIsBound);
    CHECK(document.Text(text.visibleBinding) == "{OptionsView.vsync}");
}

TEST_CASE("ScreenControls: a control can bind a context the screen declares")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <context name="boss" />
    <slider name="s" value="{=boss:Health.current}" min="0" max="1" step="1" />
</screen>)amdn");
    CHECK(document.Text(NodeNamed(document, "s").valueBinding) == "{=boss:Health.current}");
}

TEST_CASE("ScreenControls: a template passes a control its binding")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <template name="labelled_slider" params="field">
        <slider value="{=@field}" min="0" max="1" step="1" />
    </template>
    <labelled_slider name="s" field="OptionsView.volume" />
</screen>)amdn");
    CHECK(document.Text(NodeNamed(document, "s").valueBinding) == "{=OptionsView.volume}");
}

TEST_CASE("ScreenControls: on_change keeps the event, when the control can hand it what it holds")
{
    const ScreenDocument document = Bound(R"amdn(<screen>
    <toggle name="a" on_change="Game::Flipped" />
    <slider name="b" on_change="Game::Moved" min="0" max="1" step="1" />
    <stepped_slider name="c" on_change="Game::Picked" />
    <toggle name="d" on_change="Game::Saved" />
    <slider name="e" on_change="Game::Saved" min="0" max="1" step="1" />
    <stepped_slider name="f" on_change="Game::Saved" />
</screen>)amdn");
    CHECK(NodeNamed(document, "a").changeEvent.View() == "Game::Flipped");
    CHECK(NodeNamed(document, "b").changeEvent.View() == "Game::Moved");
    CHECK(NodeNamed(document, "c").changeEvent.View() == "Game::Picked");
    CHECK(NodeNamed(document, "d").changeEvent.View() == "Game::Saved");
}

TEST_CASE("ScreenControls: on_change is refused where it cannot work")
{
    CHECK(Says(RefusedInside(R"(<text on_change="Game::Saved">x</text>)"), "only a toggle"));
    CHECK(Says(RefusedInside(R"(<button on_change="Game::Saved">x</button>)"), "only a toggle"));
    CHECK(Says(RefusedInside(R"(<toggle on_change="Game::Missing" />)"), "names no event"));
    CHECK(Says(RefusedInside(R"(<toggle on_change="hide" />)"), "is a verb"));

    CHECK(Says(RefusedInside(R"(<toggle on_change="Game::Moved" />)"), "It hands its change event a bool"));
    CHECK(Says(RefusedInside(R"(<slider on_change="Game::Picked" min="0" max="1" step="1" />)"),
               "It hands its change event a float"));
    CHECK(Says(RefusedInside(R"(<stepped_slider on_change="Game::Flipped" />)"),
               "It hands its change event a int32_t"));
    CHECK(Says(RefusedInside(R"(<toggle on_change="Game::Named" />)"), "cannot fill"));
}

TEST_CASE("ScreenControls: a two-way binding is refused anywhere but a control's value")
{
    CHECK(Says(RefusedInside("<text>{=Health.current}</text>"), "{=...}"));
    CHECK(Says(RefusedInside("<button>{=Health.current}</button>"), "{=...}"));
    CHECK(Says(RefusedInside(R"(<text visible="{=Health.alive}">x</text>)"), "{=...}"));
    CHECK(Says(RefusedInside(R"(<text_field>{=Health.current}</text_field>)"), "holds a binding"));
    CHECK(Says(RefusedInside(R"(<text_field placeholder="{=Health.current}" />)"), "holds a binding"));
    CHECK(Says(RefusedInside(R"(<slider min="{=Health.current}" max="1" step="1" />)"), "'min'"));
}

TEST_CASE("ScreenControls: a control's binding is refused when the field does not suit it")
{
    CHECK(Says(RefusedInside(R"(<toggle on="{=Health.current}" />)"), "a toggle holds a bool"));
    CHECK(Says(RefusedInside(R"(<slider value="{=Health.alive}" min="0" max="1" step="1" />)"), "a slider a number"));
    CHECK(Says(RefusedInside(R"(<slider value="{=Health.stance}" min="0" max="1" step="1" />)"), "a slider a number"));
    CHECK(Says(RefusedInside(R"(<stepped_slider value="{=Health.alive}" />)"), "a whole number or an enum"));

    CHECK(Says(RefusedInside(R"(<slider value="{=Health.current:.1f}" min="0" max="1" step="1" />)"), "format"));
    CHECK(Says(RefusedInside(R"(<slider value="{Health.current:.1f}" min="0" max="1" step="1" />)"), "format"));

    CHECK(Says(RefusedInside(R"(<slider value="{=Nothing.at_all}" min="0" max="1" step="1" />)"), "not a component"));
    CHECK(Says(RefusedInside(R"(<slider value="{=OptionsView.missing}" min="0" max="1" step="1" />)"),
               "does not have"));
    CHECK(Says(RefusedInside(R"(<slider value="{=boss:Health.current}" min="0" max="1" step="1" />)"),
               "does not declare"));
    CHECK(Says(RefusedInside(R"(<slider value="words {=Health.current}" min="0" max="1" step="1" />)"),
               "other than one binding"));
}

TEST_CASE("ScreenControls: a source is on no entity, so it takes no context")
{
    CHECK(Says(RefusedInside("<context name=\"boss\" />\n<text>{boss:OptionsView.volume}</text>"), "registered source"));
}

TEST_CASE("ScreenControls: a name that is both a component and a struct cannot be bound")
{
    CHECK(Says(RefusedInside("<text>{Health.current}</text>", true), "both a component and a struct"));
}

TEST_CASE("ScreenControls: an enum decides a stepped slider's steps")
{
    CHECK(Says(RefusedInside(R"(<stepped_slider value="{=Health.stance}" steps="2" />)"), "for each enumerator"));
    CHECK(Says(RefusedInside(R"(<stepped_slider value="{=Health.stance}" max="4" />)"), "for each enumerator"));
    CHECK(Says(RefusedInside(R"(<stepped_slider min="1" value="{=Health.stance}" />)"), "for each enumerator"));

    // Whichever of a template and its instance wrote them.
    CHECK(Says(RefusedInside(R"(<template name="t"><stepped_slider value="{=Health.stance}" /></template>
<t steps="2" />)"),
               "for each enumerator"));
}

TEST_CASE("ScreenControls: a stepped slider over a whole number says how many steps it has")
{
    CHECK(Says(RefusedInside(R"(<stepped_slider value="{=Health.lives}" />)"), "with steps"));

    // A template checked alone has no value for its parameter yet.
    const ScreenDocument document = Bound(R"amdn(<screen>
    <template name="counted" params="positions">
        <stepped_slider value="{=Health.lives}" steps="@positions" />
    </template>
    <counted name="s" positions="3" />
</screen>)amdn");
    CHECK(NodeNamed(document, "s").steps == 3);
}
