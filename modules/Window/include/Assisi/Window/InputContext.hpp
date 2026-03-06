#pragma once

/// @file InputContext.hpp
/// @brief Per-frame keyboard and mouse input polling.
///
/// Constructed from a WindowContext. Call Poll() once per frame (after
/// WindowContext::PollEvents()) to snapshot input state; then query freely
/// within the same frame.
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

#include <glm/glm.hpp>

#include <Assisi/Window/Key.hpp>
#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Window
{

class InputContext
{
  public:
    /// @brief Binds this InputContext to a window.
    ///
    /// The WindowContext must outlive the InputContext.
    explicit InputContext(const WindowContext &window);

    /// @brief Snapshots the current input state.
    ///
    /// Must be called once per frame, after WindowContext::PollEvents().
    /// IsKeyPressed / IsKeyReleased / MouseDelta are only meaningful after
    /// at least one Poll() call.
    void Poll();

    // -------------------------------------------------------------------------
    // Keyboard
    // -------------------------------------------------------------------------

    /// @brief True while the key is held down.
    [[nodiscard]] bool IsKeyDown(Key key) const;

    /// @brief True on the first frame the key transitions from up to down.
    [[nodiscard]] bool IsKeyPressed(Key key) const;

    /// @brief True on the first frame the key transitions from down to up.
    [[nodiscard]] bool IsKeyReleased(Key key) const;

    // -------------------------------------------------------------------------
    // Mouse buttons
    // -------------------------------------------------------------------------

    /// @brief True while the mouse button is held down.
    [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const;

    /// @brief True on the first frame the button transitions from up to down.
    [[nodiscard]] bool IsMouseButtonPressed(MouseButton button) const;

    /// @brief True on the first frame the button transitions from down to up.
    [[nodiscard]] bool IsMouseButtonReleased(MouseButton button) const;

    // -------------------------------------------------------------------------
    // Mouse position
    // -------------------------------------------------------------------------

    /// @brief Cursor position in screen-space pixels (top-left origin).
    [[nodiscard]] glm::vec2 MousePosition() const;

    /// @brief Cursor movement since the previous Poll() call.
    [[nodiscard]] glm::vec2 MouseDelta() const;

    // -------------------------------------------------------------------------
    // Cursor mode
    // -------------------------------------------------------------------------

    /// @brief Hides and locks the cursor to the window (FPS-style).
    ///
    /// While captured, MouseDelta() returns raw movement with no screen-edge
    /// clamping. Call again with false to restore normal cursor behaviour.
    void SetMouseCaptured(bool captured);

    /// @brief Returns true if the cursor is currently captured.
    [[nodiscard]] bool IsMouseCaptured() const;

  private:
    /* GLFW_KEY_LAST = 348; one slot per key code index. */
    static constexpr int kKeyCount = 349;
    /* GLFW_MOUSE_BUTTON_LAST = 7; one slot per button index. */
    static constexpr int kButtonCount = 8;

    NativeWindowHandle *_window = nullptr;

    std::array<bool, kKeyCount> _currKeys{};
    std::array<bool, kKeyCount> _prevKeys{};

    std::array<bool, kButtonCount> _currButtons{};
    std::array<bool, kButtonCount> _prevButtons{};

    glm::vec2 _currMousePos{0.f, 0.f};
    glm::vec2 _prevMousePos{0.f, 0.f};
    glm::vec2 _mouseDelta{0.f, 0.f};

    bool _mouseCaptured = false;
};

} // namespace Assisi::Window
