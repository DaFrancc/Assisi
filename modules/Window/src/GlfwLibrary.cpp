#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <Assisi/Window/GlfwLibrary.hpp>

#include <iostream>
#include <memory>

namespace Assisi::Window
{
GlfwLibrary::GlfwLibrary()
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

GlfwLibrary::~GlfwLibrary()
{
    /* Terminate GLFW. */
    if (_isValid)
    {
        glfwTerminate();
    }
}

bool GlfwLibrary::IsValid() const
{
    return _isValid;
}

std::shared_ptr<GlfwLibrary> GlfwLibrary::Acquire()
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
} /* namespace Assisi::Window */
