/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Binding.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Sprite.hpp>
#include <Assisi/Mondrian/TestWorld.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/EventScope.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/ShortString.hpp>
#include <Assisi/Math/Color.hpp>
#include <Assisi/Math/GLM.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <typeinfo>

using namespace Assisi::Mondrian;
using Assisi::Core::AssetId;
using Assisi::Core::EventQueue;
using Assisi::Core::Reflect::ComponentMeta;
using Assisi::Core::Reflect::FieldMeta;
using Assisi::Core::Reflect::FieldType;
using Assisi::Math::ColorSpace;

namespace
{

using Srgb4 = Assisi::Math::Color4<ColorSpace::Srgb>;

constexpr Extent kViewport{1280, 720};

/// How close a colour read back after the sRGB curve must be to the expected one.
constexpr float kColorTolerance = 1e-4f;

/// Textures the tests hand out: the sheet's, and the one picture the loader knows.
constexpr TextureId kIcons{3};
constexpr TextureId kPortrait{4};
constexpr TextureId kPlaceholder{9};

/// The portrait's size in its own pixels.
constexpr uint32_t kPortraitWidth = 64;
constexpr uint32_t kPortraitHeight = 48;

/// The one id the loader below loads.
AssetId PortraitId()
{
    AssetId id;
    id.bytes[0] = 0x42;
    id.bytes[6] = 0x40; // a version 4 id, as a minted one is
    return id;
}

/// A component holding a value for each attribute a binding drives, and fields
/// of the wrong types for them.
struct Banner
{
    Srgb4 colour{0.f, 0.f, 0.f, 0.f};
    Assisi::Math::Color3<ColorSpace::Linear> glow{0.f, 0.f, 0.f};
    glm::vec4 direction{0.f};
    glm::vec3 heading{0.f};
    glm::vec2 aim{0.f};
    AssetId portrait;
    Assisi::Core::ShortString icon;
    Assisi::Core::InternedString frame;
    float fill = 0.f;
    int32_t rank = 0;
    bool raised = false;
};

FieldMeta Field(std::string name, FieldType type, std::size_t offset)
{
    FieldMeta field;
    field.name = std::move(name);
    field.type = type;
    field.offset = offset;
    return field;
}

ComponentMeta BannerMeta()
{
    return ComponentMeta{.name = "Banner",
                         .fields = {Field("colour", FieldType::SrgbColor4, offsetof(Banner, colour)),
                                    Field("glow", FieldType::LinearColor3, offsetof(Banner, glow)),
                                    Field("direction", FieldType::Vec4, offsetof(Banner, direction)),
                                    Field("heading", FieldType::Vec3, offsetof(Banner, heading)),
                                    Field("aim", FieldType::Vec2, offsetof(Banner, aim)),
                                    Field("portrait", FieldType::AssetId, offsetof(Banner, portrait)),
                                    Field("icon", FieldType::String, offsetof(Banner, icon)),
                                    Field("frame", FieldType::InternedString, offsetof(Banner, frame)),
                                    Field("fill", FieldType::Float, offsetof(Banner, fill)),
                                    Field("rank", FieldType::Int32, offsetof(Banner, rank)),
                                    Field("raised", FieldType::Bool, offsetof(Banner, raised))},
                         .typeIndex = typeid(Banner)};
}

/// Why @p written does not resolve against Banner for @p use.
BindingError Refusal(const ComponentMeta &banner, std::string_view written, BindingUse use)
{
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(written);
    REQUIRE(source.has_value());
    const std::expected<FieldBinding, BindingError> binding = ResolveBinding(*source, {.component = &banner}, use);
    REQUIRE_FALSE(binding.has_value());
    return binding.error();
}

bool Resolves(const ComponentMeta &banner, std::string_view written, BindingUse use)
{
    const std::expected<BindingSource, BindingError> source = ReadSoleBinding(written);
    REQUIRE(source.has_value());
    return ResolveBinding(*source, {.component = &banner}, use).has_value();
}

/// A sheet with two icons, side by side.
SpriteSheetDocument Icons()
{
    SpriteSheetDocument sheet;
    sheet.width = 64;
    sheet.height = 32;
    sheet.sprites = {SheetSprite{.name = Assisi::Core::InternedString{"sword"},
                                 .rect = {.x = 0, .y = 0, .width = 32, .height = 32},
                                 .borders = {}},
                     SheetSprite{.name = Assisi::Core::InternedString{"shield"},
                                 .rect = {.x = 32, .y = 0, .width = 32, .height = 32},
                                 .borders = {}}};
    return sheet;
}

constexpr EntityId kPlayer{.index = 1, .generation = 1};

/// A screen whose context is an entity holding a Banner, with one node to bind.
struct Stage
{
    EventQueue events;
    TestWorld world{events};
    EventQueue universal{Assisi::Core::EventScope::Universal};
    Ui ui{universal};
    Banner banner;
    Screen screen{ui, world, ScreenTraits{}, kSortHud, "hud"};
    NodeId box;
    uint32_t loads = 0;

    Stage()
    {
        world.components = {BannerMeta()};
        world.alive = {kPlayer};
        world.held.push_back({.component = "Banner", .data = &banner, .entity = kPlayer});
        ui.SetPlaceholderTexture(kPlaceholder);
        ui.AddSpriteSheet("ui/icons.amsp", Icons(), kIcons);
        ui.SetAssetTextureLoader(
            [this](AssetId id) -> std::optional<Sprite>
            {
                ++loads;
                return id == PortraitId() ? std::optional<Sprite>{WholeTexture(kPortrait, kPortraitWidth,
                                                                               kPortraitHeight)}
                                          : std::nullopt;
            });
        box = screen.Add(screen.Root(), Style{});
        screen.Show();
    }

    void Frame()
    {
        ui.ProcessInput({});
        ui.Sync(kViewport);
    }

    void Bind(BoundAttribute attribute, std::string_view written, Sizing length = Sizing::Fixed(Px(0.f)))
    {
        REQUIRE(screen.BindAttribute(box, attribute, written, length).has_value());
    }

    /// Makes the box float against the whole screen, its top left on the
    /// screen's top left and moved by @p offset.
    void Float(std::array<Length, kAxisCount> offset)
    {
        Style floating;
        floating.floating.enabled = true;
        floating.floating.target = FloatAnchor::Root;
        floating.floating.offset = offset;
        floating.sizing = {Sizing::Fixed(Px(10.f)), Sizing::Fixed(Px(10.f))};
        screen.Tree().SetStyle(box, floating);
    }

    [[nodiscard]] Rect Placed() const { return screen.GetLayout().Get(box)->rect; }

    [[nodiscard]] const Node &Box() const { return *screen.Tree().Get(box); }
};

bool Near(const Srgb4 &a, const Srgb4 &b)
{
    return std::abs(a.r - b.r) < kColorTolerance && std::abs(a.g - b.g) < kColorTolerance &&
           std::abs(a.b - b.b) < kColorTolerance && std::abs(a.a - b.a) < kColorTolerance;
}

} // namespace

TEST_CASE("AttributeBindings: each attribute takes only fields of the types it can show")
{
    const ComponentMeta banner = BannerMeta();

    CHECK(Resolves(banner, "{Banner.colour}", BindingUse::Colour));
    CHECK(Resolves(banner, "{Banner.glow}", BindingUse::Colour));
    CHECK(Refusal(banner, "{Banner.direction}", BindingUse::Colour) == BindingError::WrongAttribute);
    CHECK(Refusal(banner, "{Banner.fill}", BindingUse::Colour) == BindingError::WrongAttribute);

    CHECK(Resolves(banner, "{Banner.fill}", BindingUse::Length));
    CHECK(Resolves(banner, "{Banner.rank}", BindingUse::Length));
    CHECK(Refusal(banner, "{Banner.raised}", BindingUse::Length) == BindingError::WrongAttribute);

    CHECK(Resolves(banner, "{Banner.icon}", BindingUse::SpriteName));
    CHECK(Resolves(banner, "{Banner.frame}", BindingUse::SpriteName));
    CHECK(Refusal(banner, "{Banner.portrait}", BindingUse::SpriteName) == BindingError::WrongAttribute);

    CHECK(Resolves(banner, "{Banner.portrait}", BindingUse::Texture));
    CHECK(Refusal(banner, "{Banner.icon}", BindingUse::Texture) == BindingError::WrongAttribute);

    CHECK(Resolves(banner, "{Banner.aim}", BindingUse::Offset));
    CHECK(Refusal(banner, "{Banner.fill}", BindingUse::Offset) == BindingError::WrongAttribute);
    CHECK(Refusal(banner, "{Banner.heading}", BindingUse::Offset) == BindingError::WrongAttribute);
    CHECK(Refusal(banner, "{Banner.direction}", BindingUse::Offset) == BindingError::WrongAttribute);
    CHECK(Refusal(banner, "{Banner.aim:.1f}", BindingUse::Offset) == BindingError::BadFormat);
}

TEST_CASE("AttributeBindings: an attribute's binding takes no format and writes nothing back")
{
    const ComponentMeta banner = BannerMeta();
    CHECK(Refusal(banner, "{Banner.fill:.1f}", BindingUse::Length) == BindingError::BadFormat);
    CHECK(Refusal(banner, "{Banner.colour:x}", BindingUse::Colour) == BindingError::BadFormat);
    CHECK(Refusal(banner, "{=Banner.fill}", BindingUse::Length) == BindingError::TwoWayElsewhere);
}

TEST_CASE("AttributeBindings: a bound colour follows its field the frame it changes")
{
    Stage stage;
    stage.Bind(BoundAttribute::Background, "{Banner.colour}");
    stage.banner.colour = Srgb4{0.2f, 0.4f, 0.6f, 1.f};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    CHECK(Near(stage.Box().style.background, Srgb4{0.2f, 0.4f, 0.6f, 1.f}));

    stage.banner.colour = Srgb4{1.f, 0.f, 0.f, 0.5f};
    stage.Frame();
    CHECK(Near(stage.Box().style.background, Srgb4{1.f, 0.f, 0.f, 0.5f}));
}

TEST_CASE("AttributeBindings: a linear colour is shown in sRGB, opaque when it has no alpha")
{
    Stage stage;
    stage.Bind(BoundAttribute::Tint, "{Banner.glow}");
    stage.banner.glow = Assisi::Math::Color3<ColorSpace::Linear>{0.214f, 1.f, 0.f};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    const Srgb4 expected{Assisi::Math::LinearToSrgb(0.214f), 1.f, 0.f, 1.f};
    CHECK(Near(stage.Box().style.tint, expected));
}

TEST_CASE("AttributeBindings: until it reads, a bound attribute is as the node's style leaves it")
{
    Stage stage;
    Style resting;
    resting.borderColor = Srgb4{0.f, 1.f, 0.f, 1.f};
    stage.screen.Tree().SetStyle(stage.box, resting);
    stage.Bind(BoundAttribute::BorderColor, "{Banner.colour}");
    stage.Frame();
    CHECK(Near(stage.Box().style.borderColor, Srgb4{0.f, 1.f, 0.f, 1.f}));

    // With nothing left to read, it keeps what it last showed.
    stage.banner.colour = Srgb4{0.f, 0.f, 1.f, 1.f};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    stage.screen.SetContext(EntityId{});
    stage.banner.colour = Srgb4{1.f, 1.f, 1.f, 1.f};
    stage.Frame();
    CHECK(Near(stage.Box().style.borderColor, Srgb4{0.f, 0.f, 1.f, 1.f}));
}

TEST_CASE("AttributeBindings: a state's look still goes over a bound colour")
{
    Stage stage;
    StylePatch disabled;
    disabled.state = StyleState::Disabled;
    disabled.values.background = Srgb4{0.5f, 0.5f, 0.5f, 1.f};
    disabled.written = PropertyBit(StyleProperty::Background);
    stage.screen.Tree().SetStateStyle(stage.box, disabled);
    stage.Bind(BoundAttribute::Background, "{Banner.colour}");
    stage.banner.colour = Srgb4{1.f, 0.f, 0.f, 1.f};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    CHECK(Near(stage.Box().style.background, Srgb4{1.f, 0.f, 0.f, 1.f}));

    stage.screen.Tree().SetEnabled(stage.box, false);
    stage.Frame();
    CHECK(Near(stage.Box().style.background, Srgb4{0.5f, 0.5f, 0.5f, 1.f}));
}

TEST_CASE("AttributeBindings: a bound width follows a number in the unit it is bound with, within its limits")
{
    // At 1280 by 720 a UI pixel is two thirds of a device pixel, so the limit
    // is 600 device pixels: under half the screen's width, over a tenth of it.
    constexpr float kMaxUiPixels = 900.f;
    constexpr float kMaxDevicePixels = 600.f;
    constexpr float kTenthDevicePixels = 128.f;

    Stage stage;
    stage.Bind(BoundAttribute::Width, "{Banner.fill}",
               Sizing{.value = Percent(0.f), .min = {}, .max = Px(kMaxUiPixels), .kind = SizingKind::Fixed});
    // Until it reads, the width is the node's own: it grows to fit.
    stage.Frame();
    CHECK(stage.Box().style.sizing[static_cast<std::size_t>(Axis::X)].kind == SizingKind::Fit);

    stage.banner.fill = 50.f;
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    const Sizing &width = stage.Box().style.sizing[static_cast<std::size_t>(Axis::X)];
    CHECK(width.kind == SizingKind::Fixed);
    CHECK(width.value == Percent(50.f));
    CHECK(stage.screen.GetLayout().Get(stage.box)->rect.width == doctest::Approx(kMaxDevicePixels));

    stage.banner.fill = 10.f;
    stage.Frame();
    CHECK(stage.Box().style.sizing[static_cast<std::size_t>(Axis::X)].value == Percent(10.f));
    CHECK(stage.screen.GetLayout().Get(stage.box)->rect.width == doctest::Approx(kTenthDevicePixels));
}

TEST_CASE("AttributeBindings: a bound height from a whole number is fixed at it")
{
    Stage stage;
    stage.Bind(BoundAttribute::Height, "{Banner.rank}");
    stage.banner.rank = 12;
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    const Sizing &height = stage.Box().style.sizing[static_cast<std::size_t>(Axis::Y)];
    CHECK(height.kind == SizingKind::Fixed);
    CHECK(height.value == Px(12.f));
}

TEST_CASE("AttributeBindings: a bound sprite shows the sprite its field names, and nothing for none")
{
    Stage stage;
    stage.Bind(BoundAttribute::Sprite, "{Banner.icon}");
    stage.Frame();
    CHECK_FALSE(stage.Box().hasImage);

    stage.banner.icon.Assign("icons:shield");
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    REQUIRE(stage.Box().hasImage);
    CHECK(stage.Box().image == *stage.ui.FindSprite("icons:shield"));

    stage.banner.icon.Assign("icons:sword");
    stage.Frame();
    CHECK(stage.Box().image == *stage.ui.FindSprite("icons:sword"));

    // A name no sheet has, and no name, both show nothing.
    stage.banner.icon.Assign("icons:axe");
    stage.Frame();
    CHECK_FALSE(stage.Box().hasImage);
    stage.banner.icon.Assign("icons:sword");
    stage.Frame();
    stage.banner.icon.Assign("");
    stage.Frame();
    CHECK_FALSE(stage.Box().hasImage);
}

TEST_CASE("AttributeBindings: a bound background image draws the sprite its field names")
{
    Stage stage;
    stage.Bind(BoundAttribute::BackgroundImage, "{Banner.frame}");
    stage.banner.frame = Assisi::Core::InternedString{"icons:sword"};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    CHECK(stage.Box().style.backgroundImage == Assisi::Core::InternedString{"icons:sword"});
    REQUIRE(stage.Box().hasBackgroundImage);
    CHECK(stage.Box().background == *stage.ui.FindSprite("icons:sword"));
}

TEST_CASE("AttributeBindings: a bound texture loads the image its asset id names, once")
{
    Stage stage;
    stage.Bind(BoundAttribute::Texture, "{Banner.portrait}");
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    CHECK_FALSE(stage.Box().hasImage); // a nil id is no picture
    CHECK(stage.loads == 0);

    stage.banner.portrait = PortraitId();
    stage.Frame();
    REQUIRE(stage.Box().hasImage);
    CHECK(stage.Box().image.texture == kPortrait);
    CHECK(stage.Box().image.width == kPortraitWidth);

    stage.Frame();
    stage.banner.portrait = AssetId{};
    stage.Frame();
    stage.banner.portrait = PortraitId();
    stage.Frame();
    CHECK(stage.loads == 1);

    // One that cannot load shows the placeholder.
    stage.banner.portrait.bytes[1] = 0x01;
    stage.Frame();
    REQUIRE(stage.Box().hasImage);
    CHECK(stage.Box().image.texture == kPlaceholder);
}

TEST_CASE("AttributeBindings: a bound float offset moves the node the frame its field changes")
{
    // At 1280 by 720 a UI pixel is two thirds of a device pixel.
    constexpr float kDevicePerUiPixel = 2.f / 3.f;

    Stage stage;
    stage.Float({Px(0.f), Px(0.f)});
    stage.Bind(BoundAttribute::FloatOffset, "{Banner.aim}");
    stage.banner.aim = glm::vec2{30.f, 60.f};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    CHECK(stage.Box().style.floating.offset == std::array<Length, kAxisCount>{Px(30.f), Px(60.f)});
    CHECK(stage.Placed().x == doctest::Approx(30.f * kDevicePerUiPixel));
    CHECK(stage.Placed().y == doctest::Approx(60.f * kDevicePerUiPixel));

    stage.banner.aim = glm::vec2{-15.f, 90.f};
    stage.Frame();
    CHECK(stage.Placed().x == doctest::Approx(-15.f * kDevicePerUiPixel));
    CHECK(stage.Placed().y == doctest::Approx(90.f * kDevicePerUiPixel));
}

TEST_CASE("AttributeBindings: a bound float offset in % is a share of what the node floats against")
{
    Stage stage;
    stage.Float({Px(0.f), Px(0.f)});
    stage.Bind(BoundAttribute::FloatOffset, "{Banner.aim}", Sizing::Fixed(Percent(0.f)));
    stage.banner.aim = glm::vec2{25.f, 50.f};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    CHECK(stage.Box().style.floating.offset == std::array<Length, kAxisCount>{Percent(25.f), Percent(50.f)});
    CHECK(stage.Placed().x == doctest::Approx(0.25f * static_cast<float>(kViewport.width)));
    CHECK(stage.Placed().y == doctest::Approx(0.5f * static_cast<float>(kViewport.height)));

    stage.banner.aim = glm::vec2{75.f, 10.f};
    stage.Frame();
    CHECK(stage.Placed().x == doctest::Approx(0.75f * static_cast<float>(kViewport.width)));
    CHECK(stage.Placed().y == doctest::Approx(0.1f * static_cast<float>(kViewport.height)));
}

TEST_CASE("AttributeBindings: until it reads, a bound float offset is the node's own, and a state's goes over it")
{
    Stage stage;
    stage.Float({Px(6.f), Px(9.f)});
    StylePatch disabled;
    disabled.state = StyleState::Disabled;
    disabled.values.floating.offset = {Px(1.f), Px(2.f)};
    disabled.written = PropertyBit(StyleProperty::FloatOffset);
    stage.screen.Tree().SetStateStyle(stage.box, disabled);
    stage.Bind(BoundAttribute::FloatOffset, "{Banner.aim}");
    stage.Frame();
    CHECK(stage.Box().style.floating.offset == std::array<Length, kAxisCount>{Px(6.f), Px(9.f)});

    stage.banner.aim = glm::vec2{40.f, 50.f};
    stage.screen.SetContext(kPlayer);
    stage.Frame();
    CHECK(stage.Box().style.floating.offset == std::array<Length, kAxisCount>{Px(40.f), Px(50.f)});

    stage.screen.Tree().SetEnabled(stage.box, false);
    stage.Frame();
    CHECK(stage.Box().style.floating.offset == std::array<Length, kAxisCount>{Px(1.f), Px(2.f)});
}

TEST_CASE("AttributeBindings: binding an attribute the world cannot resolve is refused")
{
    Stage stage;
    CHECK(stage.screen.BindAttribute(stage.box, BoundAttribute::Background, "{Banner.fill}").error() ==
          BindingError::WrongAttribute);
    CHECK(stage.screen.BindAttribute(stage.box, BoundAttribute::Width, "{Banner.missing}").error() ==
          BindingError::UnknownField);
    CHECK(stage.screen.BindAttribute(stage.box, BoundAttribute::Width, "{Banner.fill} wide").error() ==
          BindingError::NotOneBinding);
}
