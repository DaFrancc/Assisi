/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file InputContext.hpp
/// @brief Keyboard and mouse input, gathered from events into one frame's snapshot.
///
/// The window's key, button, cursor and scroll events feed the context as they
/// arrive, and Poll() makes everything since the last Poll the current frame;
/// nothing a query returns changes until the next Poll. A key's edges say
/// whether it went down or up at any point during the frame, so a key pressed
/// and released between two polls reads as both pressed and released, and not
/// down — the tap a snapshot of held keys would miss.
///
/// Example:
/// @code
///   Assisi::Window::InputContext input(window);
///
///   while (!window.ShouldClose())
///   {
///       window.PollEvents();
///       input.Poll();
///
///       if (input.IsKeyDown(Key::W))   camera.MoveForward(speed * dt);
///       if (input.IsKeyPressed(Key::Escape)) window.RequestClose();
///   }
/// @endcode

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include <Assisi/Window/InputEvent.hpp>
#include <Assisi/Window/Key.hpp>
#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Window
{

class InputContext
{
  public:
    /// @brief Input with no window behind it, fed only through the On*() calls.
    /// Cursor capture does nothing.
    InputContext() = default;

    /// @brief Input from @p window: subscribes to its events and starts at its
    /// cursor. The WindowContext must outlive the InputContext (see
    /// WindowContext's subscriber-lifetime warning).
    explicit InputContext(WindowContext &window);

    // The window's subscriptions hold this object's address.
    InputContext(const InputContext &) = delete;
    InputContext &operator=(const InputContext &) = delete;

    /// @brief Makes every event since the last call the current frame. Call
    /// once per frame, after WindowContext::PollEvents().
    void Poll();

    // -------------------------------------------------------------------------
    // Events, as the window delivers them
    // -------------------------------------------------------------------------

    /// @brief @p key changed at @p time, in seconds on any clock that only
    /// moves forward; the time matters only for counting taps.
    void OnKey(Key key, KeyAction action, double time);
    /// @brief @p button changed at @p time; a click continues a run of taps
    /// only near where the one before it landed.
    void OnMouseButton(MouseButton button, KeyAction action, double time);
    /// @brief The cursor moved to (@p x, @p y), in window coordinates.
    void OnCursorPosition(double x, double y);
    void OnScroll(double yOffset);

    // -------------------------------------------------------------------------
    // Keyboard
    // -------------------------------------------------------------------------

    /// @brief True while the key is held down.
    [[nodiscard]] bool IsKeyDown(Key key, ConsumedInput consumed = ConsumedInput::Skip) const;

    /// @brief True in the frame the key went down.
    [[nodiscard]] bool IsKeyPressed(Key key, ConsumedInput consumed = ConsumedInput::Skip) const;

    /// @brief True in the frame the key went up.
    [[nodiscard]] bool IsKeyReleased(Key key, ConsumedInput consumed = ConsumedInput::Skip) const;

    /// @brief In the frame the key went down, which tap of a quick run that
    /// press was: 1 for a single press, 2 for a double tap, and so on without
    /// limit. Zero in a frame with no press.
    [[nodiscard]] uint32_t TapCount(Key key) const;

    // -------------------------------------------------------------------------
    // Mouse buttons
    // -------------------------------------------------------------------------

    /// @brief True while the mouse button is held down.
    [[nodiscard]] bool IsMouseButtonDown(MouseButton button, ConsumedInput consumed = ConsumedInput::Skip) const;

    /// @brief True in the frame the button went down.
    [[nodiscard]] bool IsMouseButtonPressed(MouseButton button, ConsumedInput consumed = ConsumedInput::Skip) const;

    /// @brief True in the frame the button went up.
    [[nodiscard]] bool IsMouseButtonReleased(MouseButton button, ConsumedInput consumed = ConsumedInput::Skip) const;

    /// @brief TapCount for a mouse button: 2 in the frame of a double click.
    [[nodiscard]] uint32_t ClickCount(MouseButton button) const;

    // -------------------------------------------------------------------------
    // Tap timing
    // -------------------------------------------------------------------------

    /// @brief The longest gap between presses that still continues a run of
    /// taps, in seconds. A game sets its standard here at startup, and a
    /// settings screen may hand the choice to the player. Clamped to
    /// [kMinMultiTapSeconds, kMaxMultiTapSeconds]; a value that is not a number
    /// is ignored.
    void SetMultiTapInterval(double seconds);
    [[nodiscard]] double GetMultiTapInterval() const { return _multiTapSeconds; }

    // -------------------------------------------------------------------------
    // Mouse position
    // -------------------------------------------------------------------------

    /// @brief Cursor position in window coordinates (top-left origin).
    [[nodiscard]] glm::vec2 MousePosition() const { return _framePosition; }

    /// @brief Cursor movement since the previous frame.
    [[nodiscard]] glm::vec2 MouseDelta() const { return _frameDelta; }

    /// @brief Scroll wheel movement during the frame (positive = up).
    [[nodiscard]] float ScrollDelta() const { return _frameScroll; }

    // -------------------------------------------------------------------------
    // Consumption
    // -------------------------------------------------------------------------

    /// @brief Hides @p key from every query that does not ask past consumption,
    /// from now until the frame after it is released, so the press that worked
    /// a menu is not still down for gameplay the next frame.
    void ConsumeKey(Key key);
    void ConsumeMouseButton(MouseButton button);
    /// @brief Consumes every key held, pressed or released this frame.
    void ConsumeKeyboard();
    /// @brief Consumes every button held, pressed or released this frame, and
    /// the frame's movement and scroll. The position stays readable.
    void ConsumeMouse();
    void ConsumeAll();

    // -------------------------------------------------------------------------
    // Input mode
    // -------------------------------------------------------------------------

    /// @brief The game's own mode, in force while nothing is pushed over it.
    ///
    /// Game captures and hides the cursor; MouseDelta() is then raw movement
    /// with no screen-edge clamping.
    void SetInputMode(InputMode mode);

    /// @brief Puts @p mode over the game's own until the handle is popped. The
    /// newest mode still pushed is the one in force.
    [[nodiscard]] InputModeHandle PushInputMode(InputMode mode);

    /// @brief Takes off the mode @p handle names, wherever it is in the stack.
    /// A handle already popped, or a null one, does nothing.
    void PopInputMode(InputModeHandle handle);

    /// @brief The mode in force.
    [[nodiscard]] InputMode GetInputMode() const;

    /// @brief Whether the cursor is captured, which is whether the game has
    /// both the mouse and the keyboard.
    [[nodiscard]] bool IsMouseCaptured() const { return GetInputMode() == InputMode::Game; }

  private:
    /// Captures or frees the window's cursor to match the mode in force.
    void ApplyCursorMode();

    /// A pushed mode, and the id its handle carries.
    struct PushedMode
    {
        uint32_t id = 0;
        InputMode mode = InputMode::Game;
    };

    /// One slot per GLFW key code, up to GLFW_KEY_LAST.
    static constexpr std::size_t kKeyCount = 349;
    /// One slot per GLFW mouse button, up to GLFW_MOUSE_BUTTON_LAST.
    static constexpr std::size_t kButtonCount = 8;

    /// How far the cursor may move between clicks, in window coordinates, and
    /// the second still count as part of the same run.
    static constexpr float kMultiClickSlop = 4.f;

    /// Down state, edges and tap runs for one kind of input: live as events
    /// arrive, and as of the last Poll.
    template <std::size_t Count> struct Switches
    {
        std::array<double, Count> lastPress{};
        std::array<uint32_t, Count> run{};       ///< taps in the current run
        std::array<uint32_t, Count> tapsSince{}; ///< run length at the last press since Poll
        std::array<uint32_t, Count> taps{};
        std::array<bool, Count> live{};
        std::array<bool, Count> pressedSince{};
        std::array<bool, Count> releasedSince{};
        std::array<bool, Count> down{};
        std::array<bool, Count> pressed{};
        std::array<bool, Count> released{};
        std::array<bool, Count> consumed{}; ///< hidden until the frame after release

        /// @return whether the action was a press of an input that was up.
        bool Apply(std::size_t index, KeyAction action);
        /// Counts a press at @p time, continuing the run when @p continues.
        void Tap(std::size_t index, double time, bool continues);
        void Latch();
        /// Consumes every input held, pressed or released this frame.
        void ConsumeActive();
        /// Whether @p flags holds for @p value, counting consumed input as @p counting says.
        [[nodiscard]] bool Reads(const std::array<bool, Count> &flags, int32_t value, ConsumedInput counting) const;
    };

    Switches<kKeyCount> _keys;
    Switches<kButtonCount> _buttons;
    std::array<glm::vec2, kButtonCount> _lastClickPosition{};
    std::vector<PushedMode> _pushedModes;
    NativeWindowHandle *_window = nullptr;
    double _multiTapSeconds = kDefaultMultiTapSeconds;
    glm::vec2 _livePosition{0.f, 0.f};
    glm::vec2 _framePosition{0.f, 0.f};
    glm::vec2 _frameDelta{0.f, 0.f};
    float _scrollSince = 0.f;
    float _frameScroll = 0.f;
    uint32_t _nextModeId = 1;
    InputMode _gameMode = InputMode::GameAndUi;
};

} // namespace Assisi::Window
