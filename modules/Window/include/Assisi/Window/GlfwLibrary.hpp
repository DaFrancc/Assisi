#pragma once

/// @file GlfwLibrary.hpp
/// @brief RAII guard for the GLFW library lifetime.
///
/// `GlfwLibrary` calls `glfwInit()` on construction and `glfwTerminate()` on
/// destruction.  Exactly one instance is kept alive for the whole process via
/// the `Acquire()` singleton helper; callers share ownership through
/// `std::shared_ptr<GlfwLibrary>`, so GLFW is torn down automatically when
/// the last owner is released.

#include <memory>

namespace Assisi::Window
{
/// @brief RAII owner of the GLFW library.
///
/// Non-copyable and non-movable; always accessed through a shared_ptr
/// returned by Acquire().
class GlfwLibrary
{
  public:
    /// @brief Calls `glfwInit()`.  Sets `_isValid` to false on failure.
    GlfwLibrary();

    /// @brief Calls `glfwTerminate()` if the library was successfully initialised.
    ~GlfwLibrary();

    GlfwLibrary(const GlfwLibrary &) = delete;
    GlfwLibrary &operator=(const GlfwLibrary &) = delete;

    GlfwLibrary(GlfwLibrary &&) = delete;
    GlfwLibrary &operator=(GlfwLibrary &&) = delete;

    /// @brief Returns true if `glfwInit()` succeeded.
    [[nodiscard]] bool IsValid() const;

    /// @brief Returns the process-wide shared GlfwLibrary instance.
    ///
    /// Uses a `std::weak_ptr` internally so the instance is destroyed when
    /// every caller releases its `shared_ptr`.  A new instance is created
    /// transparently if none is currently alive.
    ///
    /// @return A shared_ptr to the singleton GlfwLibrary.
    [[nodiscard]] static std::shared_ptr<GlfwLibrary> Acquire();

  private:
    /// @brief True after a successful `glfwInit()`.
    bool _isValid = false;
};
} /* namespace Assisi::Window */
