/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Sprite.hpp>

#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/ScreenLoader.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/EventCatalog.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace Assisi::Mondrian;
using Assisi::Core::InternedString;

namespace
{

constexpr Extent kViewport{1920, 1080};
constexpr TextureId kFrames{5};
constexpr TextureId kLogo{6};
constexpr TextureId kPlaceholder{7};

constexpr std::string_view kSheetPath = "ui/frames.amsp";
constexpr std::string_view kLogoPath = "ui/logo.png";

/// A 64-pixel texture holding a framed panel, the same panel framed for high
/// contrast, and a 16-pixel icon.
SpriteSheetDocument Frames()
{
    SpriteSheetDocument sheet;
    sheet.texture = Assisi::Core::AssetPath{"ui/frames.png"};
    sheet.width = 64;
    sheet.height = 64;
    sheet.sprites = {
        SheetSprite{.name = InternedString{"panel"},
                    .rect = {.x = 0, .y = 0, .width = 32, .height = 32},
                    .borders = {.left = 8, .top = 8, .right = 8, .bottom = 8}},
        SheetSprite{.name = InternedString{"panel_contrast"},
                    .rect = {.x = 32, .y = 0, .width = 32, .height = 32},
                    .borders = {.left = 4, .top = 4, .right = 4, .bottom = 4}},
        SheetSprite{.name = InternedString{"icon"},
                    .rect = {.x = 0, .y = 32, .width = 16, .height = 16},
                    .borders = {}},
    };
    return sheet;
}

/// A sprite read into the one property that can hold it, as the cook reads one.
StylePatch SpriteValue(std::string_view qualified)
{
    Style style;
    style.backgroundImage = InternedString{qualified};
    StylePatch value;
    value.Take(StyleProperty::BackgroundImage, style);
    return value;
}

/// Two modes, and a frame token giving each its own panel.
Themes FramedThemes()
{
    ThemeDocument theme;
    theme.modes = {InternedString{"dark"}, InternedString{"contrast"}};
    theme.tokens = {ThemeToken{.name = InternedString{"frame"},
                               .values = {SpriteValue("frames:panel"), SpriteValue("frames:panel_contrast")}}};
    Themes themes;
    themes.listed = {ListedTheme{.vpath = "ui/Look.amth", .document = std::move(theme)}};
    return themes;
}

/// A screen holding a 300 by 200 panel framed by the theme's token, an icon
/// from the sheet, and an image named by its path.
ScreenDocument Gallery()
{
    ScreenDocument document;
    document.traits = {.input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::NoHide, .pause = ScreenPause::Run};
    document.nodes.emplace_back();

    ScreenNode panel;
    panel.parent = 0;
    panel.name = InternedString{"panel"};
    panel.style.sizing = {Sizing::Fixed(Px(300.f)), Sizing::Fixed(Px(200.f))};
    panel.tokens = {TokenUse{.token = InternedString{"frame"}, .property = StyleProperty::BackgroundImage}};
    panel.written = PropertyBit(StyleProperty::Width) | PropertyBit(StyleProperty::Height) |
                    PropertyBit(StyleProperty::BackgroundImage);
    document.nodes.push_back(panel);

    ScreenNode icon;
    icon.parent = 0;
    icon.name = InternedString{"icon"};
    icon.sprite = InternedString{"frames:icon"};
    document.nodes.push_back(icon);

    ScreenNode logo;
    logo.parent = 0;
    logo.name = InternedString{"logo"};
    logo.texture = InternedString{kLogoPath};
    document.nodes.push_back(logo);
    return document;
}

/// A UI with the sheet and the themes, loading images by path through a loader
/// that counts its calls, and the gallery shown on it.
struct Stage
{
    Assisi::Core::EventQueue events;
    Assisi::Core::EventQueue universal{Assisi::Core::EventScope::Universal};
    Themes themes = FramedThemes();
    uint32_t loads = 0;
    Ui ui{universal};
    std::unique_ptr<Screen> screen;

    Stage()
    {
        ui.SetPlaceholderTexture(kPlaceholder);
        ui.AddSpriteSheet(kSheetPath, Frames(), kFrames);
        ui.SetThemes(&themes);
        ui.SetTextureLoader(
            [this](std::string_view vpath) -> std::optional<Sprite>
            {
                ++loads;
                return vpath == kLogoPath ? std::optional<Sprite>{WholeTexture(kLogo, 120, 40)} : std::nullopt;
            });
        screen = Load(Gallery());
        ui.Show(*screen);
        Step();
    }

    std::unique_ptr<Screen> Load(const ScreenDocument &document)
    {
        std::expected<LoadedScreen, ScreenLoadError> loaded =
            InstantiateScreen(ui, events, "ui/Gallery.amdn", document, Assisi::Core::EventCatalog{});
        REQUIRE(loaded.has_value());
        return std::move(loaded->screen);
    }

    void Step()
    {
        (void)ui.ProcessInput(UiInput{});
        ui.Sync(kViewport);
    }

    [[nodiscard]] const Node &NodeOf(std::string_view name) const
    {
        const Node *node = screen->Tree().Get(screen->Find(name));
        REQUIRE(node != nullptr);
        return *node;
    }

    [[nodiscard]] Rect RectOf(std::string_view name) const
    {
        const LayoutNode *placed = screen->GetLayout().Get(screen->Find(name));
        REQUIRE(placed != nullptr);
        return placed->rect;
    }
};

/// The first nine-slice in the UI's draw list, or null.
const QuadInstance *FirstNineSlice(const Ui &ui)
{
    const std::span<const QuadInstance> quads = ui.GetDrawList().Instances();
    const std::span<const QuadInstance>::iterator found = std::ranges::find_if(
        quads, [](const QuadInstance &quad) { return quad.kind == static_cast<uint32_t>(QuadKind::NineSlice); });
    return found != quads.end() ? &*found : nullptr;
}

} // namespace

TEST_CASE("Images: a sprite is found by its sheet's name and its own")
{
    Stage stage;
    const Sprite *icon = stage.ui.FindSprite("frames:icon");
    REQUIRE(icon != nullptr);
    CHECK(icon->texture == kFrames);
    CHECK(icon->uv.y == doctest::Approx(0.5f));
    CHECK(icon->width == 16);
    CHECK(stage.ui.FindSprite("frames:missing") == nullptr);
    CHECK(stage.ui.FindSprite("other:icon") == nullptr);
}

TEST_CASE("Images: a palette mode swaps a panel's frame")
{
    Stage stage;
    const Node &panel = stage.NodeOf("panel");
    REQUIRE(panel.hasBackgroundImage);
    CHECK(panel.background == *stage.ui.FindSprite("frames:panel"));
    const QuadInstance *dark = FirstNineSlice(stage.ui);
    REQUIRE(dark != nullptr);
    CHECK(dark->slice[2] == PackSliceInsets(8, 8));

    REQUIRE(stage.ui.SetThemeMode("contrast").has_value());
    stage.Step();
    CHECK(stage.NodeOf("panel").background == *stage.ui.FindSprite("frames:panel_contrast"));
    const QuadInstance *contrast = FirstNineSlice(stage.ui);
    REQUIRE(contrast != nullptr);
    CHECK(contrast->slice[2] == PackSliceInsets(4, 4));
}

TEST_CASE("Images: an image from a sheet shows its sprite, at its own size")
{
    Stage stage;
    const Node &icon = stage.NodeOf("icon");
    REQUIRE(icon.hasImage);
    CHECK(icon.image == *stage.ui.FindSprite("frames:icon"));
    // The reference viewport's UI pixel is one device pixel.
    CHECK(stage.RectOf("icon").width == 16.f);
    CHECK(stage.RectOf("icon").height == 16.f);
}

TEST_CASE("Images: an image named by its path is loaded once, however many screens name it")
{
    Stage stage;
    CHECK(stage.loads == 1);
    CHECK(stage.NodeOf("logo").image.texture == kLogo);
    CHECK(stage.RectOf("logo").width == 120.f);

    const std::unique_ptr<Screen> second = stage.Load(Gallery());
    CHECK(stage.loads == 1);
}

TEST_CASE("Images: a picture that cannot be found shows the placeholder")
{
    Stage stage;
    ScreenDocument document = Gallery();
    document.nodes[2].sprite = InternedString{"frames:missing"};
    document.nodes[3].texture = InternedString{"ui/missing.png"};
    const std::unique_ptr<Screen> broken = stage.Load(document);

    const Node *icon = broken->Tree().Get(broken->Find("icon"));
    const Node *logo = broken->Tree().Get(broken->Find("logo"));
    REQUIRE(icon != nullptr);
    REQUIRE(logo != nullptr);
    CHECK(icon->image.texture == kPlaceholder);
    CHECK(logo->image.texture == kPlaceholder);

    // A failed load is remembered too, so a second screen does not try again.
    const uint32_t loads = stage.loads;
    const std::unique_ptr<Screen> again = stage.Load(document);
    CHECK(stage.loads == loads);
}
