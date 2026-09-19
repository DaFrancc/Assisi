/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Window/InputContext.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

using namespace Assisi::Window;

namespace
{

/// Seconds apart that two presses are, well inside and well outside the
/// default interval.
constexpr double kQuick = 0.1;
constexpr double kSlow = 1.0;

/// Presses and releases @p key at @p time.
void Tap(InputContext &input, Key key, double time)
{
    input.OnKey(key, KeyAction::Press, time);
    input.OnKey(key, KeyAction::Release, time);
}

void Click(InputContext &input, MouseButton button, double time)
{
    input.OnMouseButton(button, KeyAction::Press, time);
    input.OnMouseButton(button, KeyAction::Release, time);
}

} // namespace

TEST_CASE("InputContext: a key pressed and released between two frames reads as both, and not down")
{
    InputContext input;
    Tap(input, Key::Space, 0.0);
    input.Poll();

    CHECK(input.IsKeyPressed(Key::Space));
    CHECK(input.IsKeyReleased(Key::Space));
    CHECK_FALSE(input.IsKeyDown(Key::Space));
}

TEST_CASE("InputContext: an edge lasts one frame and down lasts until release")
{
    InputContext input;
    input.OnKey(Key::W, KeyAction::Press, 0.0);
    input.Poll();
    CHECK(input.IsKeyPressed(Key::W));
    CHECK(input.IsKeyDown(Key::W));

    input.Poll();
    CHECK_FALSE(input.IsKeyPressed(Key::W));
    CHECK(input.IsKeyDown(Key::W));

    input.OnKey(Key::W, KeyAction::Release, 0.0);
    input.Poll();
    CHECK(input.IsKeyReleased(Key::W));
    CHECK_FALSE(input.IsKeyDown(Key::W));
}

TEST_CASE("InputContext: a held key released and pressed again within a frame reads as both edges, and down")
{
    InputContext input;
    input.OnKey(Key::A, KeyAction::Press, 0.0);
    input.Poll();
    input.OnKey(Key::A, KeyAction::Release, 0.0);
    input.OnKey(Key::A, KeyAction::Press, 0.0);
    input.Poll();

    CHECK(input.IsKeyReleased(Key::A));
    CHECK(input.IsKeyPressed(Key::A));
    CHECK(input.IsKeyDown(Key::A));
}

TEST_CASE("InputContext: a repeat is not a press")
{
    InputContext input;
    input.OnKey(Key::D, KeyAction::Press, 0.0);
    input.Poll();
    input.OnKey(Key::D, KeyAction::Repeat, kQuick);
    input.Poll();

    CHECK_FALSE(input.IsKeyPressed(Key::D));
    CHECK(input.IsKeyDown(Key::D));
    CHECK(input.TapCount(Key::D) == 0);
}

TEST_CASE("InputContext: events wait for the next frame, so a frame reads the same throughout")
{
    InputContext input;
    input.Poll();
    input.OnKey(Key::S, KeyAction::Press, 0.0);
    CHECK_FALSE(input.IsKeyDown(Key::S));
    CHECK_FALSE(input.IsKeyPressed(Key::S));

    input.Poll();
    CHECK(input.IsKeyDown(Key::S));
}

TEST_CASE("InputContext: a mouse click between two frames reads as pressed and released")
{
    InputContext input;
    Click(input, MouseButton::Left, 0.0);
    input.Poll();

    CHECK(input.IsMouseButtonPressed(MouseButton::Left));
    CHECK(input.IsMouseButtonReleased(MouseButton::Left));
    CHECK_FALSE(input.IsMouseButtonDown(MouseButton::Left));
    CHECK_FALSE(input.IsMouseButtonPressed(MouseButton::Right));
}

TEST_CASE("InputContext: the cursor's delta is its movement between frames")
{
    InputContext input;
    input.OnCursorPosition(10.0, 10.0);
    input.Poll();
    input.OnCursorPosition(12.0, 11.0);
    input.OnCursorPosition(15.0, 20.0);
    input.Poll();

    CHECK(input.MousePosition() == glm::vec2(15.f, 20.f));
    CHECK(input.MouseDelta() == glm::vec2(5.f, 10.f));

    input.Poll();
    CHECK(input.MousePosition() == glm::vec2(15.f, 20.f));
    CHECK(input.MouseDelta() == glm::vec2(0.f, 0.f));
}

TEST_CASE("InputContext: scrolling sums within a frame and resets after it")
{
    InputContext input;
    input.OnScroll(1.0);
    input.OnScroll(2.0);
    input.Poll();
    CHECK(input.ScrollDelta() == 3.f);

    input.Poll();
    CHECK(input.ScrollDelta() == 0.f);
}

TEST_CASE("InputContext: quick presses count up a run of taps, reported in the frame of each press")
{
    InputContext input;
    Tap(input, Key::E, 0.0);
    input.Poll();
    CHECK(input.TapCount(Key::E) == 1);

    input.Poll();
    CHECK(input.TapCount(Key::E) == 0); // no press this frame

    Tap(input, Key::E, kQuick);
    input.Poll();
    CHECK(input.TapCount(Key::E) == 2);
}

TEST_CASE("InputContext: a run has no limit")
{
    constexpr uint32_t kTaps = 7;
    InputContext input;
    for (uint32_t tap = 0; tap < kTaps; ++tap)
    {
        Tap(input, Key::Q, static_cast<double>(tap) * kQuick);
        input.Poll();
    }
    CHECK(input.TapCount(Key::Q) == kTaps);
}

TEST_CASE("InputContext: a slow press starts a new run")
{
    InputContext input;
    Tap(input, Key::E, 0.0);
    Tap(input, Key::E, kQuick);
    input.Poll();
    CHECK(input.TapCount(Key::E) == 2); // both taps inside one frame

    Tap(input, Key::E, kQuick + kSlow);
    input.Poll();
    CHECK(input.TapCount(Key::E) == 1);
}

TEST_CASE("InputContext: the multi-tap interval is what decides a run")
{
    constexpr double kGap = 0.5;
    InputContext input;
    CHECK(input.GetMultiTapInterval() == kDefaultMultiTapSeconds);
    Tap(input, Key::F, 0.0);
    Tap(input, Key::F, kGap);
    input.Poll();
    CHECK(input.TapCount(Key::F) == 1); // past the default

    input.SetMultiTapInterval(kGap + kQuick);
    Tap(input, Key::F, 2.0 * kGap);
    input.Poll();
    CHECK(input.TapCount(Key::F) == 2);
}

TEST_CASE("InputContext: the multi-tap interval stays within its bounds and ignores nonsense")
{
    InputContext input;
    input.SetMultiTapInterval(0.0);
    CHECK(input.GetMultiTapInterval() == kMinMultiTapSeconds);
    input.SetMultiTapInterval(60.0);
    CHECK(input.GetMultiTapInterval() == kMaxMultiTapSeconds);
    input.SetMultiTapInterval(std::numeric_limits<double>::quiet_NaN());
    CHECK(input.GetMultiTapInterval() == kMaxMultiTapSeconds);
}

TEST_CASE("InputContext: a double click counts only when the cursor has barely moved")
{
    InputContext input;
    input.OnCursorPosition(100.0, 100.0);
    Click(input, MouseButton::Left, 0.0);
    input.OnCursorPosition(101.0, 102.0);
    Click(input, MouseButton::Left, kQuick);
    input.Poll();
    CHECK(input.ClickCount(MouseButton::Left) == 2);

    input.OnCursorPosition(300.0, 100.0);
    Click(input, MouseButton::Left, 2.0 * kQuick);
    input.Poll();
    CHECK(input.ClickCount(MouseButton::Left) == 1);
}

TEST_CASE("InputContext: each key keeps its own run")
{
    InputContext input;
    Tap(input, Key::A, 0.0);
    Tap(input, Key::D, kQuick);
    input.Poll();
    CHECK(input.TapCount(Key::A) == 1);
    CHECK(input.TapCount(Key::D) == 1);
}
