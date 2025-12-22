#pragma once

#ifndef GLFWLIBRARY_HPP
#define GLFWLIBRARY_HPP

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <iostream>
#include <memory>

namespace Assisi::Window
{
class GlfwLibrary
{
  public:
    GlfwLibrary()
    {
        /* Initialize GLFW. */
        if (glfwInit() != GLFW_TRUE)
        {
            std::cout << "Failed to initialize GLFW." << std::endl;
            _isValid = false;
            return;
        }

        _isValid = true;
    }

    ~GlfwLibrary()
    {
        /* Terminate GLFW. */
        if (_isValid)
        {
            glfwTerminate();
        }
    }

    GlfwLibrary(const GlfwLibrary &) = delete;
    GlfwLibrary &operator=(const GlfwLibrary &) = delete;

    GlfwLibrary(GlfwLibrary &&) = delete;
    GlfwLibrary &operator=(GlfwLibrary &&) = delete;

    bool IsValid() const { return _isValid; }

    static std::shared_ptr<GlfwLibrary> Acquire()
    {
        /* Create exactly one shared library guard for the whole process. */
        static std::weak_ptr<GlfwLibrary> weakInstance;

        std::shared_ptr<GlfwLibrary> instance = weakInstance.lock();
        if (!instance)
        {
            instance = std::make_shared<GlfwLibrary>();
            weakInstance = instance;
        }

        return instance;
    }

  private:
    bool _isValid = false;
};
} /* namespace Assisi::Window */

#endif /* GLFWLIBRARY_HPP */