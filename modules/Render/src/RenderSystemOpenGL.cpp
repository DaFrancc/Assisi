/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Render/RenderSystem.hpp>

#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace Assisi::Render
{
bool RenderSystem::InitializeOpenGL(const Assisi::Window::WindowContext &window)
{
    /* Ensure the OpenGL context is current. */
    glfwMakeContextCurrent(window.NativeHandle());

    /* Load OpenGL function pointers using GLAD. */
    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0)
    {
        Assisi::Core::Log::Error("RenderSystem: Failed to initialize GLAD.");
        return false;
    }

    glEnable(GL_DEPTH_TEST);

    return true;
}
} /* namespace Assisi::Render */