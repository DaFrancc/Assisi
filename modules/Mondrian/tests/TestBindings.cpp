/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Binding.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Math/GLM.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <typeinfo>
#include <vector>

using namespace Assisi::Mondrian;
using Assisi::Core::EventQueue;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;

namespace
{

constexpr Extent kViewport{1280, 720};

enum class Mood : uint8_t
{
    Calm,
    Angry,
    Count_
};

/// A component of every kind text can show, and one it cannot.
struct Gauge
{
    glm::mat4 pose{1.f};
    double precise = 0.0;
    glm::vec3 velocity{0.f};
    Assisi::Core::ShortString title;
    Assisi::Core::InternedString tag;
    float speed = 0.f;
    int32_t count = 0;
    uint64_t big = 0;
    int8_t tiny = 0;
    Mood mood = Mood::Calm;
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

ComponentMeta GaugeMeta()
{
    FieldMeta mood = Field("mood", FieldType::Enum, offsetof(Gauge, mood));
    mood.enumSize = sizeof(Mood);
    mood.enumConstants = {{.name = "Calm", .value = 0}, {.name = "Angry", .value = 1}};

    return ComponentMeta{.name = "Gauge",
                         .fields = {Field("pose", FieldType::Mat4, offsetof(Gauge, pose)),
                                    Field("precise", FieldType::Double, offsetof(Gauge, precise)),
                                    Field("velocity", FieldType::Vec3, offsetof(Gauge, velocity)),
                                    Field("title", FieldType::String, offsetof(Gauge, title)),
                                    Field("tag", FieldType::InternedString, offsetof(Gauge, tag)),
                                    Field("speed", FieldType::Float, offsetof(Gauge, speed)),
                                    Field("count", FieldType::Int32, offsetof(Gauge, count)),
                                    Field("big", FieldType::UInt64, offsetof(Gauge, big)),
                                    Field("tiny", FieldType::Int8, offsetof(Gauge, tiny)),
                                    mood,
                                    Field("alive", FieldType::Bool, offsetof(Gauge, alive))},
                         .typeIndex = typeid(Gauge)};
}

/// @p written resolved against Gauge for @p use, which the case expects to
/// resolve.
FieldBinding Bound(const ComponentMeta &gauge, std::string_view written, BindingUse use = BindingUse::Text)
{
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(written);
    REQUIRE(source.has_value());
    std::expected<FieldBinding, BindingError> binding = ResolveBinding(*source, {.component = &gauge}, use);
    REQUIRE(binding.has_value());
    return *std::move(binding);
}

/// Why @p written does not resolve against Gauge for @p use.
BindingError Refusal(const ComponentMeta &gauge, std::string_view written, BindingUse use = BindingUse::Text)
{
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(written);
    REQUIRE(source.has_value());
    const ComponentMeta *const component = source->component == gauge.name ? &gauge : nullptr;
    const std::expected<FieldBinding, BindingError> binding = ResolveBinding(*source, {.component = component}, use);
    REQUIRE_FALSE(binding.has_value());
    return binding.error();
}

constexpr EntityId kPlayer{.index = 1, .generation = 1};
constexpr EntityId kOther{.index = 2, .generation = 1};

/// A HUD screen in a world holding two entities, each with a Gauge.
struct Hud
{
    EventQueue events;
    TestWorld world{events};
    EventQueue universal{Assisi::Core::EventScope::Universal};
    Ui ui{universal};
    Gauge player;
    Gauge other;
    Screen screen{ui, world, ScreenTraits{}, kSortHud, "hud"};
    NodeId text;

    Hud()
    {
        world.components = {GaugeMeta()};
        world.alive = {kPlayer, kOther};
        world.held.push_back({.component = "Gauge", .data = &player, .entity = kPlayer});
        world.held.push_back({.component = "Gauge", .data = &other, .entity = kOther});
        text = screen.AddText(screen.Root(), Style{}, "");
        screen.Show();
    }

    void Frame()
    {
        ui.ProcessInput({});
        ui.Sync(kViewport);
    }

    [[nodiscard]] std::string Shown() const { return screen.Tree().Get(text)->text; }
    [[nodiscard]] bool Visible() const { return screen.Tree().Get(text)->visible; }
};

} // namespace

TEST_CASE("Bindings: text splits into its words and its bindings")
{
    const std::expected<std::vector<TextRun>, BindingError> runs =
        SplitBoundText("Speed {Gauge.speed:.1f} m/s, {boss:Gauge.count}");
    REQUIRE(runs.has_value());
    // Nothing follows the last binding, so no run holds words after it.
    REQUIRE(runs->size() == 2);

    CHECK((*runs)[0].words == "Speed ");
    REQUIRE((*runs)[0].bound);
    CHECK((*runs)[0].binding.context.empty());
    CHECK((*runs)[0].binding.component == "Gauge");
    CHECK((*runs)[0].binding.field == "speed");
    CHECK((*runs)[0].binding.format == ".1f");

    CHECK((*runs)[1].words == " m/s, ");
    REQUIRE((*runs)[1].bound);
    CHECK((*runs)[1].binding.context == "boss");
    CHECK((*runs)[1].binding.format.empty());
}

TEST_CASE("Bindings: a doubled brace is a brace, and text with none of them is one run")
{
    const std::expected<std::vector<TextRun>, BindingError> runs = SplitBoundText("{{not bound}} at all");
    REQUIRE(runs.has_value());
    REQUIRE(runs->size() == 1);
    CHECK((*runs)[0].words == "{not bound} at all");
    CHECK_FALSE((*runs)[0].bound);
}

TEST_CASE("Bindings: text that does not read as bindings is refused with why")
{
    CHECK(SplitBoundText("{Gauge.speed").error() == BindingError::Unclosed);
    CHECK(SplitBoundText("speed}").error() == BindingError::StrayClose);
    CHECK(SplitBoundText("{speed}").error() == BindingError::BadShape);
    CHECK(SplitBoundText("{Gauge.}").error() == BindingError::BadShape);
    CHECK(SplitBoundText("{.speed}").error() == BindingError::BadShape);
    CHECK(SplitBoundText("{1Gauge.speed}").error() == BindingError::BadShape);
    CHECK(SplitBoundText("{:Gauge.speed}").error() == BindingError::BadShape);
    CHECK(SplitBoundText("{Gauge.sp eed}").error() == BindingError::BadShape);
}

TEST_CASE("Bindings: visible takes one binding and nothing around it")
{
    CHECK(ReadSoleBinding("{Gauge.alive}").has_value());
    CHECK(ReadSoleBinding("is {Gauge.alive}").error() == BindingError::NotOneBinding);
    CHECK(ReadSoleBinding("{Gauge.alive}{Gauge.alive}").error() == BindingError::NotOneBinding);
    CHECK(ReadSoleBinding("true").error() == BindingError::NotOneBinding);
}

TEST_CASE("Bindings: a binding resolves only to a field its place can show")
{
    const ComponentMeta gauge = GaugeMeta();
    CHECK(Refusal(gauge, "{Missing.speed}") == BindingError::UnknownComponent);
    CHECK(Refusal(gauge, "{Gauge.missing}") == BindingError::UnknownField);
    CHECK(Refusal(gauge, "{Gauge.pose}") == BindingError::Unshowable);
    CHECK(Refusal(gauge, "{Gauge.alive:.1f}") == BindingError::BadFormat);
    CHECK(Refusal(gauge, "{Gauge.title:.1f}") == BindingError::BadFormat);
    CHECK(Refusal(gauge, "{Gauge.speed}", BindingUse::Bool) == BindingError::NotBool);
    CHECK(Refusal(gauge, "{Gauge.alive:>4}", BindingUse::Bool) == BindingError::BadFormat);
}

TEST_CASE("Bindings: every kind of field shows as text, through its format")
{
    const ComponentMeta gauge = GaugeMeta();
    Gauge value;
    value.precise = 2.25;
    value.velocity = {1.f, -2.5f, 0.f};
    value.title.Assign("Warden");
    value.tag = Assisi::Core::InternedString{"boss"};
    value.speed = 3.14159f;
    value.count = -7;
    value.big = 1'000'000'000'000;
    value.tiny = -3;
    value.mood = Mood::Angry;
    value.alive = true;

    CHECK(ShowField(Bound(gauge, "{Gauge.speed:.2f}"), &value) == "3.14");
    CHECK(ShowField(Bound(gauge, "{Gauge.precise}"), &value) == "2.25");
    CHECK(ShowField(Bound(gauge, "{Gauge.velocity:.1f}"), &value) == "1.0, -2.5, 0.0");
    CHECK(ShowField(Bound(gauge, "{Gauge.title}"), &value) == "Warden");
    CHECK(ShowField(Bound(gauge, "{Gauge.tag:>6}"), &value) == "  boss");
    CHECK(ShowField(Bound(gauge, "{Gauge.count:+}"), &value) == "-7");
    CHECK(ShowField(Bound(gauge, "{Gauge.big}"), &value) == "1000000000000");
    CHECK(ShowField(Bound(gauge, "{Gauge.tiny}"), &value) == "-3"); // a number, not a character
    CHECK(ShowField(Bound(gauge, "{Gauge.mood}"), &value) == "Angry");
    CHECK(ShowField(Bound(gauge, "{Gauge.alive}"), &value) == "true");
    CHECK(ReadBoolField(Bound(gauge, "{Gauge.alive}", BindingUse::Bool), &value));
}

TEST_CASE("Bindings: a key labels each value of an enum or a bool from a table")
{
    const ComponentMeta gauge = GaugeMeta();
    Gauge value;
    value.mood = Mood::Angry;
    value.alive = true;

    const FieldBinding mood = Bound(gauge, "{Gauge.mood:#hud:mood}");
    CHECK(LabelOf(mood, &value) == "hud:mood.Angry");
    value.mood = Mood::Calm;
    CHECK(LabelOf(mood, &value) == "hud:mood.Calm");
    CHECK(LabelsOf(mood) == std::vector<std::string>{"hud:mood.Calm", "hud:mood.Angry"});

    const FieldBinding alive = Bound(gauge, "{Gauge.alive:#hud:flag}");
    CHECK(LabelOf(alive, &value) == "hud:flag.true");
    value.alive = false;
    CHECK(LabelOf(alive, &value) == "hud:flag.false");
    CHECK(LabelsOf(alive) == std::vector<std::string>{"hud:flag.true", "hud:flag.false"});

    // A value no enumerator names has no entry, and shows as its number.
    value.mood = Mood::Count_;
    CHECK_FALSE(LabelOf(mood, &value).has_value());
    CHECK(ShowField(mood, &value) == "2");
}

TEST_CASE("Bindings: only an enum or a bool with no key shows words from the program")
{
    const ComponentMeta gauge = GaugeMeta();
    const FieldBinding plain = Bound(gauge, "{Gauge.mood}");
    Gauge value;

    CHECK(ShowsProgramWords(plain));
    CHECK(ShowsProgramWords(Bound(gauge, "{Gauge.alive}")));
    CHECK_FALSE(ShowsProgramWords(Bound(gauge, "{Gauge.mood:#hud:mood}")));
    CHECK_FALSE(ShowsProgramWords(Bound(gauge, "{Gauge.speed}")));
    CHECK_FALSE(ShowsProgramWords(Bound(gauge, "{Gauge.title}")));

    CHECK_FALSE(LabelOf(plain, &value).has_value());
    CHECK(LabelsOf(plain).empty());
}

TEST_CASE("Bindings: a # on a number is std::format's, and a key anywhere else is refused")
{
    const ComponentMeta gauge = GaugeMeta();
    Gauge value;
    value.count = 255;
    CHECK(ShowField(Bound(gauge, "{Gauge.count:#x}"), &value) == "0xff");

    CHECK(Refusal(gauge, "{Gauge.count:#hud:mood}") == BindingError::BadFormat);
    CHECK(Refusal(gauge, "{Gauge.title:#hud:mood}") == BindingError::BadFormat);
    CHECK(Refusal(gauge, "{Gauge.velocity:#hud:mood}") == BindingError::BadFormat);

    CHECK(Refusal(gauge, "{Gauge.mood:#mood}") == BindingError::BadKey);
    CHECK(Refusal(gauge, "{Gauge.alive:#x}") == BindingError::BadKey);
    CHECK(Refusal(gauge, "{Gauge.mood:#:mood}") == BindingError::BadKey);
    CHECK(Refusal(gauge, "{Gauge.alive:#hud:flag}", BindingUse::Bool) == BindingError::BadFormat);
}

TEST_CASE("Bindings: text bound with a key shows what the UI's tables hold, and follows them")
{
    Hud hud;
    StringTables english;
    english.byName["hud"].entries = {{"mood.Calm", "Calm"}, {"mood.Angry", "Furious"}};
    StringTables french;
    french.byName["hud"].entries = {{"mood.Calm", "Paisible"}};
    hud.ui.SetStringTables(&english);

    REQUIRE(hud.screen.BindText(hud.text, "Mood: {Gauge.mood:#hud:mood}").has_value());
    hud.player.mood = Mood::Angry;
    hud.screen.SetContext(kPlayer);
    hud.Frame();
    CHECK(hud.Shown() == "Mood: Furious");

    hud.player.mood = Mood::Calm;
    hud.Frame();
    CHECK(hud.Shown() == "Mood: Calm");

    hud.ui.SetStringTables(&french);
    hud.Frame();
    CHECK(hud.Shown() == "Mood: Paisible");

    // No entry in a stale package shows the key, as a key set from code does.
    hud.player.mood = Mood::Angry;
    hud.Frame();
    CHECK(hud.Shown() == "Mood: #hud:mood.Angry");
}

TEST_CASE("Bindings: an enum value no enumerator names shows as its number")
{
    const ComponentMeta gauge = GaugeMeta();
    Gauge value;
    value.mood = Mood::Count_;
    CHECK(ShowField(Bound(gauge, "{Gauge.mood}"), &value) == "2");
}

TEST_CASE("Bindings: a value that shows as zero shows without a sign")
{
    // A speed a hair below zero flickering between "-0.0" and "0.0" reads as
    // a fault.
    const ComponentMeta gauge = GaugeMeta();
    Gauge value;
    value.speed = -0.04f;
    value.velocity = {-0.f, -0.01f, -0.2f};
    CHECK(ShowField(Bound(gauge, "{Gauge.speed:.1f}"), &value) == "0.0");
    CHECK(ShowField(Bound(gauge, "{Gauge.velocity:.1f}"), &value) == "0.0, 0.0, -0.2");
}

TEST_CASE("Bindings: bound text follows its context's entity at every sync")
{
    Hud hud;
    REQUIRE(hud.screen.BindText(hud.text, "Speed {Gauge.speed:.1f}").has_value());

    // Nothing to read yet: the words, and nothing in the binding's place.
    hud.Frame();
    CHECK(hud.Shown() == "Speed ");

    hud.player.speed = 4.f;
    hud.screen.SetContext(kPlayer);
    hud.Frame();
    CHECK(hud.Shown() == "Speed 4.0");

    // The frame it changes.
    hud.player.speed = 5.5f;
    hud.Frame();
    CHECK(hud.Shown() == "Speed 5.5");

    // Another entity, and every binding reads it.
    hud.other.speed = 9.f;
    hud.screen.SetContext(kOther);
    hud.Frame();
    CHECK(hud.Shown() == "Speed 9.0");
}

TEST_CASE("Bindings: a binding whose entity has gone keeps what it last showed")
{
    Hud hud;
    REQUIRE(hud.screen.BindText(hud.text, "{Gauge.count}").has_value());
    hud.player.count = 3;
    hud.screen.SetContext(kPlayer);
    hud.Frame();
    REQUIRE(hud.Shown() == "3");

    SUBCASE("destroyed")
    {
        hud.world.alive = {kOther};
        hud.player.count = 99; // freed memory, as far as the screen knows
        hud.Frame();
        CHECK(hud.Shown() == "3");
    }

    SUBCASE("destroyed after the input step, before the sync")
    {
        // The frame's systems run between the two: the context has not been
        // told yet, so the sync's own look is all that stops the read.
        hud.ui.ProcessInput({});
        hud.world.alive = {kOther};
        hud.player.count = 99;
        hud.ui.Sync(kViewport);
        CHECK(hud.Shown() == "3");
    }

    SUBCASE("cleared")
    {
        hud.screen.SetContext(EntityId{});
        hud.Frame();
        CHECK(hud.Shown() == "3");
    }

    SUBCASE("without the component")
    {
        hud.world.held.clear();
        hud.Frame();
        CHECK(hud.Shown() == "3");
    }
}

TEST_CASE("Bindings: a named context feeds only the bindings that name it")
{
    Hud hud;
    REQUIRE(hud.screen.BindText(hud.text, "{Gauge.count}/{boss:Gauge.count}").has_value());
    hud.player.count = 1;
    hud.other.count = 2;
    hud.screen.SetContext(kPlayer);
    hud.screen.SetContext("boss", kOther);
    hud.Frame();
    CHECK(hud.Shown() == "1/2");
}

TEST_CASE("Bindings: a bound visible hides its node until it reads, then follows the bool")
{
    Hud hud;
    REQUIRE(hud.screen.BindVisible(hud.text, "{Gauge.alive}").has_value());
    CHECK_FALSE(hud.Visible());

    hud.player.alive = true;
    hud.screen.SetContext(kPlayer);
    hud.Frame();
    CHECK(hud.Visible());

    hud.player.alive = false;
    hud.Frame();
    CHECK_FALSE(hud.Visible());

    // Nothing to read: as it last was.
    hud.player.alive = true;
    hud.screen.SetContext(EntityId{});
    hud.Frame();
    CHECK_FALSE(hud.Visible());
}

TEST_CASE("Bindings: a binding the world cannot resolve is refused, and the node is left alone")
{
    Hud hud;
    hud.screen.Tree().SetText(hud.text, "as it was");
    CHECK(hud.screen.BindText(hud.text, "{Gauge.missing}").error() == BindingError::UnknownField);
    CHECK(hud.screen.BindText(hud.text, "{Gauge.speed").error() == BindingError::Unclosed);
    CHECK(hud.screen.BindVisible(hud.text, "{Gauge.speed}").error() == BindingError::NotBool);

    hud.screen.SetContext(kPlayer);
    hud.Frame();
    CHECK(hud.Shown() == "as it was");
    CHECK(hud.Visible());
}
