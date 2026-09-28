/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/CookedBlob.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace Assisi::Mondrian;

namespace
{

using Color = Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb>;
using Assisi::Core::InternedString;

/// A document with nothing left at its default, so a field the writer and the
/// reader disagree about shows up as a difference rather than as two defaults
/// that happen to match.
ScreenDocument Everything()
{
    ScreenDocument document;
    document.sortKey = kSortPopup + 7;
    document.traits = {
        .input = ScreenInput::LockedConsumeInput, .beneath = ScreenBeneath::HidesBeneath, .pause = ScreenPause::Pause};
    document.systems = {InternedString{"PauseMenu"}, InternedString{"Options"}};
    document.focus = 2;

    ScreenNode root;
    root.name = InternedString{"root"};
    root.style.background = Color{0.1f, 0.2f, 0.3f, 0.55f};
    root.style.childAlign = {Alignment::Center, Alignment::End};
    root.style.direction = Direction::Column;
    root.blocksPointer = true;
    root.visible = false;
    root.enabled = false;
    root.takesKeyboard = true;
    root.selectable = true;
    // kUnbounded is the default max and a float that has to survive the
    // round-trip exactly: a sizing whose max came back merely close would clamp
    // every node that grows.
    root.style.sizing[0] = Sizing::Fixed(Percent(50.f));
    root.style.sizing[1] = Sizing::Grow();
    document.nodes.push_back(root);

    ScreenNode panel;
    panel.parent = 0;
    panel.name = InternedString{"panel"};
    panel.styleName = InternedString{"Panel"};
    // Every unit appears at least once, so a unit the writer and the reader
    // number differently shows up as a difference.
    panel.style.sizing[0] = Sizing::Fixed(Px(420.f));
    panel.style.sizing[1] = Sizing::Fit();
    panel.style.sizing[1].min = Vh(10.f);
    panel.style.sizing[1].max = Vw(40.f);
    panel.style.padding = Padding{.left = Px(1.f), .top = Percent(2.f), .right = Vw(3.f), .bottom = Em(4.f)};
    panel.style.gap = Vh(2.f);
    panel.style.borderWidth = Px(2.f);
    panel.style.borderColor = Color{0.34f, 0.38f, 0.48f, 1.f};
    panel.style.cornerRadius = Percent(16.f);
    panel.style.cornerStyle = CornerStyle::Cut;
    panel.style.floating = Floating{.offset = {Vw(5.f), Em(6.f)},
                                    .anchor = {Alignment::End, Alignment::Center},
                                    .attach = {Alignment::Center, Alignment::End},
                                    .target = FloatAnchor::Root,
                                    .enabled = true,
                                    .clipToParent = true};
    panel.style.scrollSmoothing = 0.25f;
    panel.style.scrollBarMinLength = Px(30.f);
    panel.style.enabledScrollBars = {true, true};
    panel.style.scrollBarVisibility = ScrollBarVisibility::Always;
    panel.style.scrollBarDrag = ScrollBarDrag::Smoothed;
    panel.style.textAlign = TextAlign::Right;
    panel.style.textColor = Color{0.9f, 0.8f, 0.7f, 0.6f};
    panel.style.textSize = Em(1.5f);
    document.nodes.push_back(panel);

    ScreenNode resume;
    resume.parent = 1;
    resume.name = InternedString{"resume"};
    resume.text = document.AddText("Resume");
    resume.widget = BuiltinWidget::Button;
    resume.action = ActionKind::Verb;
    resume.verb = ScreenVerb::Hide;
    document.nodes.push_back(resume);

    ScreenNode quit;
    quit.parent = 1;
    quit.name = InternedString{"quit"};
    quit.text = document.AddText("Quit");
    quit.widget = BuiltinWidget::Button;
    quit.action = ActionKind::Event;
    quit.eventName = InternedString{"Assisi::App::QuitRequested"};
    document.nodes.push_back(quit);

    // Before the slider it moves, so the target points forward.
    ScreenNode quieter;
    quieter.parent = 1;
    quieter.name = InternedString{"quieter"};
    quieter.widget = BuiltinWidget::Button;
    quieter.action = ActionKind::Verb;
    quieter.verb = ScreenVerb::Step;
    quieter.target = 5;
    quieter.moves = -3;
    document.nodes.push_back(quieter);

    ScreenNode volume;
    volume.parent = 1;
    volume.name = InternedString{"volume"};
    volume.widget = BuiltinWidget::ContinuousSlider;
    volume.range = {.min = 0.f, .max = 100.f, .step = 5.f};
    document.nodes.push_back(volume);

    ScreenNode port;
    port.parent = 1;
    port.name = InternedString{"port"};
    port.widget = BuiltinWidget::TextField;
    port.text = document.AddText("8080");
    port.placeholder = document.AddText("pause:hint");
    port.placeholderIsKey = true;
    port.pattern = document.AddText("[0-9]+");
    port.lines = TextLines::Multi;
    port.height = TextHeight::Exactly;
    port.lineLimit = 3;
    port.maxLength = 5;
    port.mask = TextMask::Dots;
    port.check = TextCheck::Refuse;
    document.nodes.push_back(port);

    return document;
}

/// Where the layout hash starts: past the envelope and the version byte.
std::size_t LayoutHashOffset(std::span<const std::byte> bytes)
{
    Assisi::Core::BitReader reader{bytes};
    const std::expected<Assisi::Core::CookedKind, Assisi::Core::CookedBlobError> kind =
        Assisi::Core::ReadCookedHeader(reader);
    REQUIRE(kind.has_value());
    return reader.BitsRead() / 8 + sizeof(kScreenPayloadVersion);
}

std::vector<std::byte> Cook(const ScreenDocument &document)
{
    Assisi::Core::BitWriter writer;
    WriteCookedScreen(writer, document);
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

} // namespace

TEST_CASE("ScreenBlob: a document survives the round trip field for field")
{
    const ScreenDocument written = Everything();
    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(written));

    REQUIRE(read.has_value());
    CHECK(*read == written);
    // And each text reads back through the pool it was written into.
    CHECK(read->Text(read->nodes[3].text) == "Quit");
    CHECK(read->Text(read->nodes[6].pattern) == "[0-9]+");
}

TEST_CASE("ScreenBlob: a blob cooked against another field layout is refused")
{
    // What a field added to Style or ScreenNode does to every blob cooked
    // before it: the fields that follow would be read into the wrong places.
    std::vector<std::byte> bytes = Cook(Everything());
    const std::size_t offset = LayoutHashOffset(bytes);
    bytes[offset] ^= std::byte{1};

    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(bytes);
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedScreenError::UnsupportedVersion);
}

TEST_CASE("ScreenBlob: a text handle past the pool is refused")
{
    // It would read as empty, hiding a blob that disagrees with itself as a
    // blank label.
    ScreenDocument document;
    document.nodes.emplace_back();
    document.nodes[0].text = Assisi::Core::PooledString{.offset = 0, .length = 4};

    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedScreenError::Invalid);
}

TEST_CASE("ScreenBlob: an unbounded maximum comes back unbounded")
{
    // The default max is FLOAT_MAX, and a sizing whose max came back merely
    // close would clamp every node that grows.
    ScreenDocument written;
    written.nodes.emplace_back();
    written.nodes[0].style.sizing[0].max = Px(kUnbounded);

    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(written));
    REQUIRE(read.has_value());
    CHECK(read->nodes[0].style.sizing[0].max.value == kUnbounded);
}

TEST_CASE("ScreenBlob: truncation at any length is refused and never asserts")
{
    const std::vector<std::byte> whole = Cook(Everything());

    for (std::size_t length = 0; length < whole.size(); ++length)
    {
        const std::span<const std::byte> cut{whole.data(), length};
        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(cut);
        // Every short read is a refusal of some kind. Which kind depends on
        // where it landed — inside the envelope it is not a screen at all — so
        // the claim is that none of them is a success.
        REQUIRE_FALSE(read.has_value());
    }

    // The positive control: the whole thing still reads, so the loop above was
    // not passing because the document never read in the first place.
    CHECK(ReadCookedScreen(whole).has_value());
}

TEST_CASE("ScreenBlob: a payload version this build does not read is refused")
{
    std::vector<std::byte> bytes = Cook(Everything());

    // The version sits directly after the envelope, which is the one byte this
    // test has to know about.
    Assisi::Core::BitReader reader{bytes};
    const std::expected<Assisi::Core::CookedKind, Assisi::Core::CookedBlobError> kind =
        Assisi::Core::ReadCookedHeader(reader);
    REQUIRE(kind.has_value());
    const std::size_t versionOffset = reader.BitsRead() / 8;

    bytes[versionOffset] = static_cast<std::byte>(kScreenPayloadVersion + 1);
    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(bytes);

    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedScreenError::UnsupportedVersion);
}

TEST_CASE("ScreenBlob: a blob of another kind is not a screen")
{
    Assisi::Core::BitWriter writer;
    Assisi::Core::WriteCookedHeader(writer, Assisi::Core::CookedKind::Font);
    writer.WriteUInt8(kScreenPayloadVersion);
    const std::span<const std::byte> bytes = writer.Data();

    const std::expected<ScreenDocument, CookedScreenError> read =
        ReadCookedScreen(std::vector<std::byte>{bytes.begin(), bytes.end()});
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedScreenError::NotAScreen);
}

TEST_CASE("ScreenBlob: a table that is not a walkable tree is refused")
{
    SUBCASE("no root")
    {
        const ScreenDocument empty;
        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(empty));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a parent at or past its own child")
    {
        // How a cycle would arrive. The loader builds the tree in one pass over
        // the table, so a forward parent is a child attached to nothing.
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes.emplace_back();
        document.nodes[1].parent = 1;

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a root with a parent")
    {
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].parent = 0;

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a focus naming no node")
    {
        ScreenDocument document;
        document.nodes.emplace_back();
        document.focus = 4;

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a target naming no node")
    {
        // The loader indexes its table of built ids by this, so one past the
        // end is a read out of bounds rather than a button that does nothing.
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].target = 4;

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }
}

TEST_CASE("ScreenBlob: an enumerator this build does not have is refused")
{
    SUBCASE("a widget")
    {
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].widget = static_cast<BuiltinWidget>(static_cast<uint32_t>(BuiltinWidget::Count) + 3);

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a style enumerator")
    {
        // Layout switches over these, so one out of range is a case no switch
        // covers rather than a node that merely looks wrong.
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].style.direction = static_cast<Direction>(9);

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a length's unit")
    {
        // Layout switches over the unit to resolve a length, so it is checked
        // with the style's enumerators.
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].style.gap = Length{.value = 1.f, .unit = static_cast<LengthUnit>(9)};

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a screen trait")
    {
        ScreenDocument document;
        document.nodes.emplace_back();
        document.traits.input = static_cast<ScreenInput>(9);

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }

    SUBCASE("a field's own enumerator")
    {
        // The text field switches over these the way layout switches over a
        // style's, so the same check has to reach them.
        ScreenDocument document;
        document.nodes.emplace_back();
        document.nodes[0].mask = static_cast<TextMask>(9);

        const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == CookedScreenError::Invalid);
    }
}

TEST_CASE("ScreenBlob: an event action with no name is refused")
{
    // A button that resolved to nothing would silently do nothing, which is the
    // failure the whole catalog exists to prevent.
    ScreenDocument document;
    document.nodes.emplace_back();
    document.nodes[0].action = ActionKind::Event;

    const std::expected<ScreenDocument, CookedScreenError> read = ReadCookedScreen(Cook(document));
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == CookedScreenError::Invalid);
}
