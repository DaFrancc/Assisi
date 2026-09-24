/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/SampleScreen.hpp>

#include <Assisi/Mondrian/Style.hpp>
#include <Assisi/Mondrian/Ui.hpp>

#include <Assisi/Core/Assert.hpp>

#include <expected>
#include <string_view>

namespace Assisi::Mondrian
{
namespace
{

constexpr Math::Color4<Math::ColorSpace::Srgb> kPanelColor{0.10f, 0.11f, 0.14f, 0.94f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kPanelBorder{0.34f, 0.38f, 0.48f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kAccent{0.90f, 0.20f, 0.10f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kWhite{1.f, 1.f, 1.f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kBodyText{0.80f, 0.82f, 0.86f, 1.f};
constexpr Math::Color4<Math::ColorSpace::Srgb> kListColor{0.06f, 0.07f, 0.09f, 1.f};
constexpr Rect kWholeTexture{.x = 0.f, .y = 0.f, .width = 1.f, .height = 1.f};

constexpr Length kPanelWidth = Px(720.f);
constexpr Length kPanelPadding = Px(32.f);
constexpr Length kPanelGap = Px(20.f);
constexpr Length kPanelRadius = Px(16.f);
constexpr Length kPanelBorderWidth = Px(2.f);
constexpr Length kPictureSide = Px(72.f);
constexpr Length kPictureRadius = Px(12.f);
constexpr Length kTitleSize = Px(48.f);
constexpr Length kBodySize = Px(24.f);
constexpr Length kButtonSize = Px(28.f);
constexpr Length kButtonRadius = Px(10.f);
constexpr Padding kButtonPadding{.left = Px(28.f), .top = Px(10.f), .right = Px(28.f), .bottom = Px(10.f)};
constexpr SliderRange kSliderRange{.min = 0.f, .max = 100.f, .step = 5.f};
constexpr float kSliderStart = 60.f;
constexpr SliderRange kStepRange{.min = 0.f, .max = 3.f, .step = 1.f};
constexpr int32_t kSliderSteps = 4;
constexpr int32_t kSliderStep = 1;
constexpr Length kListHeight = Px(150.f);
constexpr float kScrollSmoothing = 0.12f;
constexpr Length kBadgeSize = Px(20.f);
constexpr Length kBadgeRadius = Px(16.f);
constexpr Padding kBadgePadding{.left = Px(14.f), .top = Px(4.f), .right = Px(14.f), .bottom = Px(4.f)};

/// One field of many lines for each way of being tall, side by side so that
/// typing into them shows what the three do differently. Their text says which
/// is which and stays short: the row is as tall as its tallest field.
constexpr uint32_t kNoteLines = 3;
constexpr std::string_view kGrowing = "grows forever";
constexpr std::string_view kUpTo = "up to 3 lines";
constexpr std::string_view kExactly = "exactly 3 lines";

/// The paragraph, spelled in bytes so the source file's encoding cannot change
/// it. It reads: "A retained tree of boxes, laid out in passes: widths first,
/// text wraps to them, then heights and positions. Resize the window and
/// everything reflows, with every edge on a whole pixel. Crème brûlée."
/// (A hex escape runs on through any hex digit, hence the split before an e.)
constexpr std::string_view kParagraph =
    "A retained tree of boxes, laid out in passes: widths first, text wraps to them, then heights and "
    "positions. Resize the window and everything reflows, with every edge on a whole pixel. "
    "Cr\xC3\xA8me br\xC3\xBBl\xC3\xA9"
    "e.";

/// The id @p named holds. Every name in this sample is a literal no other node
/// here shares, so a refusal is a mistake in this file.
template <typename Id> Id Named(const std::expected<Id, NameError> &named)
{
    ASSISI_ASSERT(named.has_value(), "the sample screen gave two nodes one name");
    return named.value_or(Id{});
}

/// A button called @p label: the label with padding around it, over @p look.
void AddButton(Screen &screen, NodeId row, std::string_view label, const Style &look)
{
    Style style = look;
    style.padding = kButtonPadding;
    style.textSize = kButtonSize;
    const ButtonId id = Named(screen.AddButton(row, label, label));
    screen.Tree().SetStyle(id.node, style);
}

/// The panel, its header, paragraph and buttons.
NodeId BuildPanel(Screen &screen, TextureId picture)
{
    Style root;
    root.childAlign = {Alignment::Center, Alignment::Center};
    screen.Tree().SetStyle(screen.Root(), root);

    Style panel;
    panel.sizing = {Sizing::Fixed(kPanelWidth), Sizing::Fit()};
    panel.direction = Direction::Column;
    panel.padding = Padding::All(kPanelPadding);
    panel.gap = kPanelGap;
    panel.background = kPanelColor;
    panel.borderWidth = kPanelBorderWidth;
    panel.borderColor = kPanelBorder;
    panel.cornerRadius = kPanelRadius;
    panel.cornerStyle = CornerStyle::Rounded;
    const NodeId panelId = Named(screen.Add(screen.Root(), panel, "panel"));
    screen.Tree().SetBlocksPointer(panelId, true);

    Style header;
    header.sizing = {Sizing::Grow(), Sizing::Fit()};
    header.gap = kPanelGap;
    header.childAlign = {Alignment::Start, Alignment::Center};
    const NodeId headerId = Named(screen.Add(panelId, header, "header"));

    Style pictureStyle;
    pictureStyle.sizing = {Sizing::Fixed(kPictureSide), Sizing::Fixed(kPictureSide)};
    pictureStyle.cornerRadius = kPictureRadius;
    pictureStyle.cornerStyle = CornerStyle::Rounded;
    const NodeId pictureId = Named(screen.Add(headerId, pictureStyle, "picture"));
    screen.Tree().SetImage(pictureId, picture, kWholeTexture);

    Style title;
    title.sizing = {Sizing::Grow(), Sizing::Fit()};
    title.textSize = kTitleSize;
    Named(screen.AddText(headerId, title, "Mondrian", "title"));

    Style body;
    body.sizing = {Sizing::Grow(), Sizing::Fit()};
    body.textSize = kBodySize;
    body.textColor = kBodyText;
    Named(screen.AddText(panelId, body, kParagraph, "body"));

    Style buttons;
    buttons.sizing = {Sizing::Grow(), Sizing::Fit()};
    buttons.gap = kPanelGap;
    buttons.childAlign = {Alignment::End, Alignment::Center};
    const NodeId buttonsId = Named(screen.Add(panelId, buttons, "buttons"));

    Style outlined;
    outlined.borderWidth = kPanelBorderWidth;
    outlined.borderColor = kWhite;
    outlined.cornerRadius = kButtonRadius;
    outlined.cornerStyle = CornerStyle::Rounded;
    AddButton(screen, buttonsId, "Quit", outlined);

    Style filled;
    filled.background = kAccent;
    filled.cornerRadius = kButtonRadius;
    filled.cornerStyle = CornerStyle::Cut;
    AddButton(screen, buttonsId, "Resume", filled);

    Style badge;
    badge.padding = kBadgePadding;
    badge.background = kAccent;
    badge.cornerRadius = kBadgeRadius;
    badge.cornerStyle = CornerStyle::Rounded;
    badge.textSize = kBadgeSize;
    badge.floating.enabled = true;
    badge.floating.anchor = {Alignment::End, Alignment::Start};
    badge.floating.attach = {Alignment::Center, Alignment::Center};
    Named(screen.AddText(panelId, badge, "NEW", "badge"));

    return panelId;
}

/// The controls under the panel: one of every built-in.
void BuildControls(Screen &screen, NodeId panel)
{
    Style row;
    row.sizing = {Sizing::Grow(), Sizing::Fit()};
    row.gap = kPanelGap;
    row.childAlign = {Alignment::Start, Alignment::Center};
    const NodeId controls = Named(screen.Add(panel, row, "controls"));
    screen.AddToggle(controls, true);
    const ContinuousSliderId volume = screen.AddContinuousSlider(controls, kSliderRange, kSliderStart);
    screen.SetButtons(volume, SliderButtons::Shown);
    screen.AddSteppedSlider(controls, kStepRange, kSliderSteps, kSliderStep);

    Style fields = row;
    const NodeId fieldRow = Named(screen.Add(panel, fields, "fields"));
    const TextFieldId name = screen.AddTextField(fieldRow, TextLines::Single);
    screen.SetPlaceholder(name, "your name");
    screen.SetText(name, "type here");
    const TextFieldId secret = screen.AddTextField(fieldRow, TextLines::Single);
    screen.SetPlaceholder(secret, "password");
    screen.SetText(secret, "hunter2");
    screen.SetMask(secret, TextMask::Dots);

    Style notes = fields;
    notes.childAlign = {Alignment::Start, Alignment::Start};
    const NodeId noteRow = Named(screen.Add(panel, notes, "notes"));

    // Their prompts say which is which, so emptying one does not lose the only
    // thing telling them apart.
    const TextFieldId growing = screen.AddTextField(noteRow, TextLines::Multi);
    screen.SetHeight(growing, TextHeight::Unbounded, 0);
    screen.SetPlaceholder(growing, kGrowing);
    screen.SetText(growing, kGrowing);

    const TextFieldId upTo = screen.AddTextField(noteRow, TextLines::Multi);
    screen.SetHeight(upTo, TextHeight::UpTo, kNoteLines);
    screen.SetPlaceholder(upTo, kUpTo);
    screen.SetText(upTo, kUpTo);

    const TextFieldId exactly = screen.AddTextField(noteRow, TextLines::Multi);
    screen.SetHeight(exactly, TextHeight::Exactly, kNoteLines);
    screen.SetPlaceholder(exactly, kExactly);
    screen.SetText(exactly, kExactly);

    Style list;
    list.sizing = {Sizing::Grow(), Sizing::Fixed(kListHeight)};
    list.direction = Direction::Column;
    list.background = kListColor;
    list.cornerRadius = kButtonRadius;
    list.cornerStyle = CornerStyle::Rounded;
    list.scrollBarVisibility = ScrollBarVisibility::WhenNeeded;
    list.scrollSmoothing = kScrollSmoothing;
    const NodeId scroller = screen.AddScroll(panel, list, {false, true});
    const std::expected<void, NameError> listed = screen.Tree().SetName(scroller, "list");
    ASSISI_ASSERT(listed.has_value(), "the sample screen gave two nodes one name");

    Style entry;
    entry.sizing = {Sizing::Grow(), Sizing::Fit()};
    entry.padding = kButtonPadding;
    entry.textSize = kButtonSize;
    for (const std::string_view label : {"One", "Two", "Three", "Four", "Five"})
    {
        const ButtonId id = Named(screen.AddButton(scroller, label, label));
        screen.Tree().SetStyle(id.node, entry);
    }
}

} // namespace

std::unique_ptr<Screen> AddSampleScreen(Ui &ui, TextureId picture)
{
    // A dialog rather than a menu: it is looked at beside whatever is up, so it
    // has no business hiding the rest of the UI to be seen, and the world it is
    // being looked at over goes on running.
    std::unique_ptr<Screen> screen = std::make_unique<Screen>(
        ui,
        ScreenTraits{.input = ScreenInput::ConsumeInput, .beneath = ScreenBeneath::NoHide, .pause = ScreenPause::Run},
        kSortPopup, std::string{kSampleScreenName});
    const NodeId panel = BuildPanel(*screen, picture);
    BuildControls(*screen, panel);
    return screen;
}

} // namespace Assisi::Mondrian
