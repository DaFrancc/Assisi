/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "PauseMenu.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Mondrian/Screen.hpp>
#include <Assisi/Mondrian/Style.hpp>
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <array>
#include <memory>
#include <string>

namespace Game
{
namespace
{

using namespace Assisi::Mondrian;

constexpr Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> kPanelColor{0.10f, 0.11f, 0.14f, 0.94f};
constexpr Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> kPanelBorder{0.34f, 0.38f, 0.48f, 1.f};
constexpr Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> kAccent{0.90f, 0.20f, 0.10f, 1.f};
constexpr Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> kWhite{1.f, 1.f, 1.f, 1.f};
/// The sheet drawn over the whole viewport, dimming the game behind the menu.
constexpr Assisi::Math::Color4<Assisi::Math::ColorSpace::Srgb> kScrim{0.f, 0.f, 0.f, 0.55f};

/// Lengths in logical pixels.
constexpr float kPanelWidth = 420.f;
constexpr float kPanelPadding = 32.f;
constexpr float kPanelGap = 20.f;
constexpr float kPanelRadius = 16.f;
constexpr float kPanelBorderWidth = 2.f;
constexpr float kTitleSize = 48.f;
constexpr float kButtonSize = 28.f;
constexpr float kButtonRadius = 10.f;
constexpr Padding kButtonPadding{.left = 28.f, .top = 10.f, .right = 28.f, .bottom = 10.f};

} // namespace

std::unique_ptr<Assisi::Mondrian::Screen> BuildPauseMenu(Assisi::Mondrian::Ui &ui)
{
    std::unique_ptr<Screen> screen = std::make_unique<Screen>(ui,
                                                              ScreenTraits{.input = ScreenInput::ConsumeInput,
                                                                           .beneath = ScreenBeneath::HidesBeneath,
                                                                           .pause = ScreenPause::Pause},
                                                              kSortMenu, std::string{kPauseScreenName});

    // The root covers the viewport, so the scrim does too and a click anywhere
    // outside the panel is still the menu's rather than the game's.
    Style root;
    root.childAlign = {Alignment::Center, Alignment::Center};
    root.background = kScrim;
    screen->Tree().SetStyle(screen->Root(), root);
    screen->Tree().SetBlocksPointer(screen->Root(), true);

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
    panel.childAlign = {Alignment::Center, Alignment::Start};
    const NodeId panelId = screen->Add(screen->Root(), panel, "panel");

    Style title;
    title.sizing = {Sizing::Grow(), Sizing::Fit()};
    title.textSize = kTitleSize;
    screen->AddText(panelId, title, "Paused", "title");

    Style buttons;
    buttons.sizing = {Sizing::Grow(), Sizing::Fit()};
    buttons.gap = kPanelGap;
    buttons.childAlign = {Alignment::Center, Alignment::Center};
    const NodeId buttonsId = screen->Add(panelId, buttons, "buttons");

    Style filled;
    filled.padding = kButtonPadding;
    filled.textSize = kButtonSize;
    filled.background = kAccent;
    filled.cornerRadius = kButtonRadius;
    filled.cornerStyle = CornerStyle::Rounded;
    const ButtonId resume = screen->AddButton(buttonsId, "Resume");
    screen->Tree().SetStyle(resume.node, filled);
    // Carried, not announced: closing the menu touches nothing but the UI, so
    // it needs no event, no system, and nothing named in a level file. Resume
    // works in any level that shows this screen.
    screen->OnActivate(resume.node, [](Screen &self) { self.Hide(); });

    Style outlined;
    outlined.padding = kButtonPadding;
    outlined.textSize = kButtonSize;
    outlined.borderWidth = kPanelBorderWidth;
    outlined.borderColor = kWhite;
    outlined.cornerRadius = kButtonRadius;
    outlined.cornerStyle = CornerStyle::Rounded;
    const ButtonId quit = screen->AddButton(buttonsId, "Quit");
    screen->Tree().SetStyle(quit.node, outlined);
    screen->OnActivate(quit.node, Assisi::App::QuitRequested{});

    // Focus starts on Resume, so a player reaching for the keyboard is one
    // press from carrying on rather than one press from leaving.
    ui.SetFocus(*screen, resume.node);
    return screen;
}

void PauseMenuScreenSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr) // headless host: nothing to show it in
    {
        return;
    }
    // The opener is what this world needs installed to work the menu. The
    // closer is not: Resume carries what it does, so it works whether or not
    // anything below is running.
    const std::array<std::string, 1> needs{"PauseMenu"};
    Assisi::App::AddScreen(ctx.world, BuildPauseMenu(*ctx.ui), needs);
}

void PauseMenuSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.ui == nullptr || ctx.input == nullptr) // headless host: no UI, no devices
    {
        return;
    }

    Screen *const pause = Assisi::App::FindScreen(ctx.world, kPauseScreenName);
    if (pause == nullptr)
    {
        return; // this level did not ask for one
    }

    if (ctx.input->IsKeyPressed(Assisi::Window::Key::Escape))
    {
        ctx.input->ConsumeKey(Assisi::Window::Key::Escape);
        pause->Show();
    }
}

} // namespace Game
