/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <GLFW/glfw3.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Window/WindowContext.hpp>

namespace Assisi::Window
{
namespace
{

/// The window library's name for @p shape. Arrow never reaches here: the
/// default is no cursor object at all rather than an arrow-shaped one.
int32_t GlfwCursorShape(Core::CursorShape shape)
{
    switch (shape)
    {
    case Core::CursorShape::Text:
        return GLFW_IBEAM_CURSOR;
    case Core::CursorShape::Hand:
        return GLFW_POINTING_HAND_CURSOR;
    case Core::CursorShape::Crosshair:
        return GLFW_CROSSHAIR_CURSOR;
    case Core::CursorShape::ResizeX:
        return GLFW_RESIZE_EW_CURSOR;
    case Core::CursorShape::ResizeY:
        return GLFW_RESIZE_NS_CURSOR;
    case Core::CursorShape::ResizeFall:
        return GLFW_RESIZE_NWSE_CURSOR;
    case Core::CursorShape::ResizeRise:
        return GLFW_RESIZE_NESW_CURSOR;
    case Core::CursorShape::Move:
        return GLFW_RESIZE_ALL_CURSOR;
    case Core::CursorShape::NotAllowed:
        return GLFW_NOT_ALLOWED_CURSOR;
    case Core::CursorShape::Arrow:
    case Core::CursorShape::Count:
        break;
    }
    return GLFW_ARROW_CURSOR;
}

} // namespace

WindowContext::WindowContext(const WindowConfiguration &configuration) : _glfwLibrary(GlfwLibrary::Acquire())
{
    if (!_glfwLibrary || !_glfwLibrary->IsValid())
    {
        return;
    }

    /* Vulkan owns presentation — GLFW must not create a client API context. */
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_DECORATED, configuration.Undecorated ? GLFW_FALSE : GLFW_TRUE);

    _nativeWindowHandle =
        glfwCreateWindow(configuration.Width, configuration.Height, configuration.Title, nullptr, nullptr);

    if (_nativeWindowHandle == nullptr)
    {
        Assisi::Core::Log::Error("Failed to create GLFW window.");
        return;
    }

    InstallCallbacks();

    _isValid = true;
}

void WindowContext::InstallCallbacks()
{
    // WindowContext owns the user pointer; the ImGui GLFW backend deliberately
    // does not use it (it keys off the ImGui context), so this is safe. ImGui,
    // initialised later with install_callbacks=true, chains to the callbacks
    // installed here rather than replacing them.
    glfwSetWindowUserPointer(_nativeWindowHandle, this);
    glfwSetFramebufferSizeCallback(_nativeWindowHandle, FramebufferSizeTrampoline);
    glfwSetScrollCallback(_nativeWindowHandle, ScrollTrampoline);
    glfwSetWindowRefreshCallback(_nativeWindowHandle, WindowRefreshTrampoline);
    glfwSetKeyCallback(_nativeWindowHandle, KeyTrampoline);
    glfwSetCharCallback(_nativeWindowHandle, CharacterTrampoline);
    glfwSetMouseButtonCallback(_nativeWindowHandle, MouseButtonTrampoline);
    glfwSetCursorPosCallback(_nativeWindowHandle, CursorPositionTrampoline);
}

namespace
{

static_assert(static_cast<int>(KeyAction::Release) == GLFW_RELEASE);
static_assert(static_cast<int>(KeyAction::Press) == GLFW_PRESS);
static_assert(static_cast<int>(KeyAction::Repeat) == GLFW_REPEAT);

Modifiers DecodeModifiers(int mods)
{
    return Modifiers{.shift = (mods & GLFW_MOD_SHIFT) != 0,
                     .control = (mods & GLFW_MOD_CONTROL) != 0,
                     .alt = (mods & GLFW_MOD_ALT) != 0,
                     .super = (mods & GLFW_MOD_SUPER) != 0,
                     .capsLock = (mods & GLFW_MOD_CAPS_LOCK) != 0,
                     .numLock = (mods & GLFW_MOD_NUM_LOCK) != 0};
}

WindowContext *Owner(GLFWwindow *window)
{
    return static_cast<WindowContext *>(glfwGetWindowUserPointer(window));
}

} // namespace

void WindowContext::KeyTrampoline(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    WindowContext *self = Owner(window);
    if (self == nullptr || key == GLFW_KEY_UNKNOWN)
    {
        return;
    }
    const KeyEvent event{.time = glfwGetTime(),
                         .key = static_cast<Key>(key),
                         .scancode = scancode,
                         .action = static_cast<KeyAction>(action),
                         .modifiers = DecodeModifiers(mods)};
    for (const auto &callback : self->_keyCallbacks)
    {
        callback(event);
    }
}

void WindowContext::CharacterTrampoline(GLFWwindow *window, unsigned int codepoint)
{
    if (WindowContext *self = Owner(window))
    {
        for (const auto &callback : self->_characterCallbacks)
        {
            callback(static_cast<char32_t>(codepoint));
        }
    }
}

void WindowContext::MouseButtonTrampoline(GLFWwindow *window, int button, int action, int mods)
{
    if (WindowContext *self = Owner(window))
    {
        const MouseButtonEvent event{.time = glfwGetTime(),
                                     .button = static_cast<MouseButton>(button),
                                     .action = static_cast<KeyAction>(action),
                                     .modifiers = DecodeModifiers(mods)};
        for (const auto &callback : self->_mouseButtonCallbacks)
        {
            callback(event);
        }
    }
}

void WindowContext::CursorPositionTrampoline(GLFWwindow *window, double x, double y)
{
    if (WindowContext *self = Owner(window))
    {
        for (const auto &callback : self->_cursorPositionCallbacks)
        {
            callback(x, y);
        }
    }
}

void WindowContext::OnKey(std::function<void(const KeyEvent &)> callback)
{
    _keyCallbacks.push_back(std::move(callback));
}

void WindowContext::OnCharacter(std::function<void(char32_t)> callback)
{
    _characterCallbacks.push_back(std::move(callback));
}

void WindowContext::OnMouseButton(std::function<void(const MouseButtonEvent &)> callback)
{
    _mouseButtonCallbacks.push_back(std::move(callback));
}

void WindowContext::OnCursorPosition(std::function<void(double, double)> callback)
{
    _cursorPositionCallbacks.push_back(std::move(callback));
}

void WindowContext::SetCursorShape(Core::CursorShape shape)
{
    if (_nativeWindowHandle == nullptr || shape == _cursorShape || shape == Core::CursorShape::Count)
    {
        return;
    }

    // The default is the absence of a cursor object rather than an arrow-shaped
    // one. A window that owns a cursor has it painted back over a pointer the
    // game has locked, under compositors that repaint on their own schedule.
    if (shape == Core::CursorShape::Arrow)
    {
        _cursorShape = shape;
        glfwSetCursor(_nativeWindowHandle, nullptr);
        return;
    }

    const std::size_t index = static_cast<std::size_t>(shape);
    if (_cursors[index] == nullptr)
    {
        _cursors[index] = glfwCreateStandardCursor(GlfwCursorShape(shape));
    }
    if (_cursors[index] == nullptr)
    {
        // A shape this platform's cursor theme does not carry: leave the
        // pointer as it was rather than dropping it back to the arrow, which
        // would flicker as it moved on and off whatever asked for the shape.
        return;
    }
    _cursorShape = shape;
    glfwSetCursor(_nativeWindowHandle, _cursors[index]);
}

std::string WindowContext::GetClipboardText() const
{
    if (_nativeWindowHandle == nullptr)
    {
        return {};
    }
    // Null when the clipboard holds nothing that converts to text.
    const char *text = glfwGetClipboardString(_nativeWindowHandle);
    return text != nullptr ? std::string(text) : std::string{};
}

void WindowContext::SetClipboardText(std::string_view text) const
{
    if (_nativeWindowHandle == nullptr)
    {
        return;
    }
    // GLFW takes a terminated string, which a view need not be.
    const std::string terminated(text);
    glfwSetClipboardString(_nativeWindowHandle, terminated.c_str());
}

void WindowContext::FramebufferSizeTrampoline(GLFWwindow *window, int width, int height)
{
    if (auto *self = static_cast<WindowContext *>(glfwGetWindowUserPointer(window)))
    {
        for (const auto &callback : self->_framebufferSizeCallbacks)
            callback(width, height);
    }
}

void WindowContext::ScrollTrampoline(GLFWwindow *window, double xOffset, double yOffset)
{
    if (auto *self = static_cast<WindowContext *>(glfwGetWindowUserPointer(window)))
    {
        for (const auto &callback : self->_scrollCallbacks)
            callback(xOffset, yOffset);
    }
}

void WindowContext::WindowRefreshTrampoline(GLFWwindow *window)
{
    if (auto *self = static_cast<WindowContext *>(glfwGetWindowUserPointer(window)))
    {
        for (const auto &callback : self->_windowRefreshCallbacks)
            callback();
    }
}

void WindowContext::OnFramebufferSize(std::function<void(int, int)> callback)
{
    _framebufferSizeCallbacks.push_back(std::move(callback));
}

void WindowContext::OnScroll(std::function<void(double, double)> callback)
{
    _scrollCallbacks.push_back(std::move(callback));
}

void WindowContext::OnWindowRefresh(std::function<void()> callback)
{
    _windowRefreshCallbacks.push_back(std::move(callback));
}

WindowContext::~WindowContext()
{
    DestroyCursors();
    if (_nativeWindowHandle != nullptr)
    {
        glfwDestroyWindow(_nativeWindowHandle);
    }
}

void WindowContext::DestroyCursors()
{
    for (NativeCursorHandle *&cursor : _cursors)
    {
        if (cursor != nullptr)
        {
            glfwDestroyCursor(cursor);
            cursor = nullptr;
        }
    }
}

WindowContext::WindowContext(WindowContext &&other) noexcept
    : _glfwLibrary(std::move(other._glfwLibrary)), _nativeWindowHandle(other._nativeWindowHandle),
      _isValid(other._isValid), _framebufferSizeCallbacks(std::move(other._framebufferSizeCallbacks)),
      _scrollCallbacks(std::move(other._scrollCallbacks)),
      _windowRefreshCallbacks(std::move(other._windowRefreshCallbacks)), _keyCallbacks(std::move(other._keyCallbacks)),
      _characterCallbacks(std::move(other._characterCallbacks)),
      _mouseButtonCallbacks(std::move(other._mouseButtonCallbacks)),
      _cursorPositionCallbacks(std::move(other._cursorPositionCallbacks)), _cursors(other._cursors),
      _cursorShape(other._cursorShape)
{
    other._nativeWindowHandle = nullptr;
    other._isValid = false;
    other._cursors.fill(nullptr);

    // The GLFW user pointer still points at 'other'; re-seat it on this object
    // so the callback trampolines dispatch to the moved-to subscriber lists.
    if (_nativeWindowHandle != nullptr)
    {
        glfwSetWindowUserPointer(_nativeWindowHandle, this);
    }
}

WindowContext &WindowContext::operator=(WindowContext &&other) noexcept
{
    if (this != &other)
    {
        DestroyCursors();
        if (_nativeWindowHandle != nullptr)
        {
            glfwDestroyWindow(_nativeWindowHandle);
        }

        _glfwLibrary = std::move(other._glfwLibrary);
        _nativeWindowHandle = other._nativeWindowHandle;
        _isValid = other._isValid;
        _cursors = other._cursors;
        _cursorShape = other._cursorShape;
        other._cursors.fill(nullptr);
        _framebufferSizeCallbacks = std::move(other._framebufferSizeCallbacks);
        _scrollCallbacks = std::move(other._scrollCallbacks);
        _windowRefreshCallbacks = std::move(other._windowRefreshCallbacks);
        _keyCallbacks = std::move(other._keyCallbacks);
        _characterCallbacks = std::move(other._characterCallbacks);
        _mouseButtonCallbacks = std::move(other._mouseButtonCallbacks);
        _cursorPositionCallbacks = std::move(other._cursorPositionCallbacks);

        other._nativeWindowHandle = nullptr;
        other._isValid = false;

        if (_nativeWindowHandle != nullptr)
        {
            glfwSetWindowUserPointer(_nativeWindowHandle, this);
        }
    }

    return *this;
}

bool WindowContext::IsValid() const
{
    return _isValid;
}

NativeWindowHandle *WindowContext::NativeHandle() const
{
    return _nativeWindowHandle;
}

void WindowContext::PollEvents()
{
    glfwPollEvents();
}

// The GLFW calls below dereference the window handle unchecked, so each entry
// point guards against a failed-construction or moved-from WindowContext
// (null handle) rather than trusting every caller to check IsValid() first.

bool WindowContext::ShouldClose() const
{
    // A window that doesn't exist reads as "close": callers loop on
    // !ShouldClose(), and spinning forever on a dead window is the worse bug.
    return _nativeWindowHandle == nullptr || glfwWindowShouldClose(_nativeWindowHandle) != 0;
}

bool WindowContext::IsFocused() const
{
    return _nativeWindowHandle != nullptr && glfwGetWindowAttrib(_nativeWindowHandle, GLFW_FOCUSED) != 0;
}

void WindowContext::RequestClose() const
{
    if (_nativeWindowHandle == nullptr)
    {
        return;
    }
    glfwSetWindowShouldClose(_nativeWindowHandle, GLFW_TRUE);
}

void WindowContext::SetTitle(const std::string &title) const
{
    if (_nativeWindowHandle == nullptr)
    {
        return;
    }
    glfwSetWindowTitle(_nativeWindowHandle, title.c_str());
}

WindowSize WindowContext::GetWindowSize() const
{
    int windowWidth = 0;
    int windowHeight = 0;

    if (_nativeWindowHandle != nullptr)
    {
        glfwGetWindowSize(_nativeWindowHandle, &windowWidth, &windowHeight);
    }

    WindowSize result;
    result.Width = windowWidth;
    result.Height = windowHeight;
    return result;
}

WindowSize WindowContext::GetFramebufferSize() const
{
    int framebufferWidth = 0;
    int framebufferHeight = 0;

    if (_nativeWindowHandle != nullptr)
    {
        glfwGetFramebufferSize(_nativeWindowHandle, &framebufferWidth, &framebufferHeight);
    }

    WindowSize result;
    result.Width = framebufferWidth;
    result.Height = framebufferHeight;
    return result;
}
} /* namespace Assisi::Window */
