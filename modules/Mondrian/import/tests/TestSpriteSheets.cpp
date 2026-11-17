/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "ScreenTesting.hpp"

#include <Assisi/Mondrian/Import/ScreenRules.hpp>
#include <Assisi/Mondrian/Import/SpriteSheetCompiler.hpp>

#include <Assisi/Mondrian/Sprite.hpp>

#include <Assisi/Core/BitStream.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi::Mondrian;
using namespace Assisi::Mondrian::Import;
using namespace ScreenTesting;

namespace
{

/// A real image, read in place: 256 pixels square.
constexpr std::string_view kImagePath = "ui/checker.png";
constexpr uint32_t kImageSide = 256;

constexpr std::string_view kSheetPath = "ui/frames.amsp";

constexpr std::string_view kSheet = R"amsp(<sheet texture="ui/checker.png">
  <sprite name="whole" />
  <sprite name="panel" rect="0 0 48 32" borders="8" />
  <sprite name="sides" rect="48 0 16 16" borders="1 2 3 4" />
</sheet>)amsp";

std::string ImageBytes()
{
    std::ifstream file{ASSISI_TEST_IMAGE_PATH, std::ios::binary};
    REQUIRE(file.good());
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

/// The image, under the path a sheet names it by.
Files WithImage()
{
    return Files{{std::string{kImagePath}, ImageBytes()}};
}

SpriteSheetDocument Sheet(std::string_view text)
{
    std::expected<SpriteSheetDocument, MarkupError> sheet = CompileSpriteSheetSource(text, Serving(WithImage()));
    REQUIRE_MESSAGE(sheet.has_value(), Why(sheet));
    return std::move(*sheet);
}

MarkupError SheetRefused(std::string_view text)
{
    const std::expected<SpriteSheetDocument, MarkupError> sheet = CompileSpriteSheetSource(text, Serving(WithImage()));
    REQUIRE_FALSE(sheet.has_value());
    return sheet.error();
}

/// Rules holding the sheet above as `frames`, and able to read the image.
ScreenRules FramesRules()
{
    ScreenRules rules;
    rules.sheets.push_back(ProjectSheet{.vpath = std::string{kSheetPath}, .document = Sheet(kSheet)});
    rules.read = Serving(WithImage());
    return rules;
}

/// A project listing the sheet above and the theme @p theme, with the image
/// the sheet is cut from.
Files WithTheme(std::string theme)
{
    Files files = WithImage();
    files.emplace("config/ui.json", R"({ "version": 1, "type": "UiConfig", "themes": ["ui/Look.amth"],
                                         "spriteSheets": ["ui/frames.amsp"] })");
    files.emplace(std::string{kSheetPath}, std::string{kSheet});
    files.emplace("ui/Look.amth", std::move(theme));
    return files;
}

/// A screen holding one image called "picture" whose attributes are @p attributes.
std::string ImageScreen(std::string_view attributes)
{
    return "<screen><image name=\"picture\" " + std::string{attributes} + " /></screen>";
}

} // namespace

TEST_CASE("Sprite sheets: a sprite is its rectangle and borders, the whole texture when it gives no rectangle")
{
    const SpriteSheetDocument sheet = Sheet(kSheet);
    CHECK(sheet.texture.View() == kImagePath);
    CHECK(sheet.width == kImageSide);
    CHECK(sheet.height == kImageSide);

    const SheetSprite *whole = sheet.Find("whole");
    REQUIRE(whole != nullptr);
    CHECK(whole->rect == SpriteRect{.x = 0, .y = 0, .width = kImageSide, .height = kImageSide});
    CHECK_FALSE(whole->borders.Any());

    const SheetSprite *panel = sheet.Find("panel");
    REQUIRE(panel != nullptr);
    CHECK(panel->rect == SpriteRect{.x = 0, .y = 0, .width = 48, .height = 32});
    CHECK(panel->borders == SpriteBorders{.left = 8, .top = 8, .right = 8, .bottom = 8});

    // Four borders are written left, top, right, bottom, as padding is.
    const SheetSprite *sides = sheet.Find("sides");
    REQUIRE(sides != nullptr);
    CHECK(sides->borders == SpriteBorders{.left = 1, .top = 2, .right = 3, .bottom = 4});
}

TEST_CASE("Sprite sheets: a sprite's place in its texture is the fraction of the texture it covers")
{
    const SpriteSheetDocument sheet = Sheet(kSheet);
    const Sprite sprite = SpriteOf(sheet, *sheet.Find("sides"), TextureId{3});
    CHECK(sprite.uv.x == doctest::Approx(48.f / 256.f));
    CHECK(sprite.uv.width == doctest::Approx(16.f / 256.f));
    CHECK(sprite.width == 16);
    CHECK(sprite.texture == TextureId{3});
}

TEST_CASE("Sprite sheets: a rectangle reaching past its texture fails the compile")
{
    const MarkupError error = SheetRefused(R"(<sheet texture="ui/checker.png">
  <sprite name="off" rect="250 0 16 16" />
</sheet>)");
    CHECK(error.message.find("reaches past the texture") != std::string::npos);
    CHECK(error.line == 2);
}

TEST_CASE("Sprite sheets: borders that do not fit inside their sprite fail the compile")
{
    const MarkupError error = SheetRefused(R"(<sheet texture="ui/checker.png">
  <sprite name="tight" rect="0 0 16 16" borders="8 0 9 0" />
</sheet>)");
    CHECK(error.message.find("do not fit inside their sprite") != std::string::npos);
}

TEST_CASE("Sprite sheets: a malformed sheet names its fault")
{
    CHECK(SheetRefused("<theme />").message.find("starts with <sheet>") != std::string::npos);
    CHECK(SheetRefused("<sheet />").message.find("names the image") != std::string::npos);
    CHECK(SheetRefused(R"(<sheet texture="ui/missing.png" />)").message.find("could not be read") !=
          std::string::npos);
    CHECK(SheetRefused(R"(<sheet texture="ui/checker.png"><sprite rect="0 0 4 4" /></sheet>)")
          .message.find("needs a name") != std::string::npos);
    CHECK(SheetRefused(R"(<sheet texture="ui/checker.png"><sprite name="a:b" /></sheet>)")
          .message.find("holds no ':'") != std::string::npos);
    CHECK(SheetRefused(R"(<sheet texture="ui/checker.png"><sprite name="a" /><sprite name="a" /></sheet>)")
          .message.find("already has a sprite 'a'") != std::string::npos);
    CHECK(SheetRefused(R"(<sheet texture="ui/checker.png"><sprite name="a" size="4" /></sheet>)")
          .message.find("'size' is not part of a sprite") != std::string::npos);
    CHECK(SheetRefused(R"(<sheet texture="ui/checker.png"><sprite name="a" rect="0 0 4" /></sheet>)")
          .message.find("four whole numbers") != std::string::npos);

    const std::expected<SpriteSheetDocument, MarkupError> notImage = CompileSpriteSheetSource(
        R"(<sheet texture="ui/words.png" />)", Serving({{"ui/words.png", "not an image at all"}}));
    REQUIRE_FALSE(notImage.has_value());
    CHECK(notImage.error().message.find("is not an image") != std::string::npos);
}

TEST_CASE("Sprite sheets: a cooked sheet reads back as it was written, and one that cannot be drawn is refused")
{
    const std::expected<std::vector<std::byte>, MarkupError> cooked =
        CompileSpriteSheetText(kSheet, Serving(WithImage()));
    REQUIRE_MESSAGE(cooked.has_value(), Why(cooked));
    const std::expected<SpriteSheetDocument, CookedSpriteSheetError> read = ReadCookedSpriteSheet(*cooked);
    REQUIRE(read.has_value());
    CHECK(*read == Sheet(kSheet));

    SpriteSheetDocument past = Sheet(kSheet);
    past.sprites[1].rect.x = kImageSide;
    Assisi::Core::BitWriter writer;
    WriteCookedSpriteSheet(writer, past);
    const std::span<const std::byte> bytes = writer.Data();
    const std::expected<SpriteSheetDocument, CookedSpriteSheetError> refused = ReadCookedSpriteSheet(bytes);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == CookedSpriteSheetError::Invalid);
}

TEST_CASE("Images: an image names a sprite a listed sheet has, or an image file, and is fitted and tinted")
{
    const ScreenDocument byName = Compiled(ImageScreen(R"(sprite="frames:panel" fit="contain" tint="#ff0000")"), {},
                                           FramesRules());
    const ScreenNode &picture = NodeNamed(byName, "picture");
    CHECK(picture.sprite.View() == "frames:panel");
    CHECK(picture.texture.Empty());
    CHECK(picture.style.fit == ImageFit::Contain);
    CHECK(picture.style.tint.g == 0.f);
    CHECK(picture.written == (PropertyBit(StyleProperty::Fit) | PropertyBit(StyleProperty::Tint)));

    const ScreenDocument byPath = Compiled(ImageScreen(R"(texture="ui/checker.png")"), {}, FramesRules());
    CHECK(NodeNamed(byPath, "picture").texture.View() == kImagePath);
}

TEST_CASE("Images: a sprite no listed sheet has, or an image that is not there, fails the compile")
{
    CHECK(Refused(ImageScreen(R"(sprite="icons:heart")"), {}, FramesRules())
          .message.find("there is no sprite sheet 'icons'") != std::string::npos);
    CHECK(Refused(ImageScreen(R"(sprite="frames:heart")"), {}, FramesRules())
          .message.find("has no sprite 'heart'") != std::string::npos);
    CHECK(Refused(ImageScreen(R"(sprite="heart")"), {}, FramesRules()).message.find("which is no sprite") !=
          std::string::npos);
    CHECK(Refused(ImageScreen(R"(texture="ui/missing.png")"), {}, FramesRules()).message.find("could not be read") !=
          std::string::npos);
}

TEST_CASE("Images: an image shows one picture, so it names one")
{
    CHECK(Refused(ImageScreen(""), {}, FramesRules()).message.find("an image shows a picture") != std::string::npos);
    CHECK(Refused(ImageScreen(R"(sprite="frames:panel" texture="ui/checker.png")"), {}, FramesRules())
          .message.find("one picture") != std::string::npos);
}

TEST_CASE("Images: a background image names a sprite a listed sheet has, at rest and in a state")
{
    const ScreenDocument document = Compiled(
        R"(<screen><column name="panel" background_image="frames:panel" hover:background_image="frames:sides" />
</screen>)",
        {}, FramesRules());
    const ScreenNode &panel = NodeNamed(document, "panel");
    CHECK(panel.style.backgroundImage.View() == "frames:panel");
    REQUIRE(panel.states.size() == 1);
    CHECK(panel.states[0].patch.values.backgroundImage.View() == "frames:sides");

    CHECK(Refused(R"(<screen><column background_image="frames:missing" /></screen>)", {}, FramesRules())
          .message.find("has no sprite 'missing'") != std::string::npos);
    CHECK(Refused(R"(<screen><column hover:background_image="other:panel" /></screen>)", {}, FramesRules())
          .message.find("there is no sprite sheet 'other'") != std::string::npos);
}

TEST_CASE("Images: a theme's token can hold a sprite, and every sprite a theme names must be in a listed sheet")
{
    const Files good = WithTheme(R"(<theme>
  <palette mode="dark" frame="frames:panel" />
  <palette mode="contrast" frame="frames:sides" />
</theme>)");
    const std::expected<ScreenRules, MarkupError> rules = LoadScreenRules(Serving(good));
    REQUIRE_MESSAGE(rules.has_value(), Why(rules));
    const ScreenDocument document =
        Compiled(R"(<screen><column name="panel" background_image="$frame" /></screen>)", good, *rules);
    const ScreenNode &panel = NodeNamed(document, "panel");
    REQUIRE(panel.tokens.size() == 1);
    CHECK(panel.tokens[0].property == StyleProperty::BackgroundImage);

    const std::expected<ScreenRules, MarkupError> missing = LoadScreenRules(Serving(WithTheme(R"(<theme>
  <palette mode="dark" frame="frames:panel" />
  <palette mode="contrast" frame="frames:gone" />
</theme>)")));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().file == "ui/Look.amth");
    CHECK(missing.error().message.find("$frame, in the mode 'contrast'") != std::string::npos);

    const std::expected<ScreenRules, MarkupError> styled = LoadScreenRules(Serving(WithTheme(R"(<theme>
  <palette mode="dark" ink="#ffffff" />
  <style name="card" pressed:background_image="frames:gone" />
</theme>)")));
    REQUIRE_FALSE(styled.has_value());
    CHECK(styled.error().message.find("the style 'card'") != std::string::npos);
}
