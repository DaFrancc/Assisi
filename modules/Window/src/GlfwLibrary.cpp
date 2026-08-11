/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <GLFW/glfw3.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Window/GlfwLibrary.hpp>

#include <cstdint>
#include <memory>

namespace Assisi::Window
{
namespace
{
void GlfwErrorCallback(int code, const char *description)
{
    Assisi::Core::Log::Error("GLFW error 0x{:08X}: {}", static_cast<uint32_t>(code),
                             description != nullptr ? description : "(no description)");
}
} // namespace

GlfwLibrary::GlfwLibrary()
{
    // Installed before glfwInit so init failures report their reason too;
    // without a callback GLFW discards all error detail silently.
    glfwSetErrorCallback(GlfwErrorCallback);

    /* Initialize GLFW. */
    if (glfwInit() != GLFW_TRUE)
    {
        Assisi::Core::Log::Error("Failed to initialize GLFW.");
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
