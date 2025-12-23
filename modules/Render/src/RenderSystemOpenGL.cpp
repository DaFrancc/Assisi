/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Render/RenderSystem.hpp>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <iostream>

namespace Assisi::Render
{
bool RenderSystem::InitializeOpenGL(const Assisi::Window::WindowContext &window)
{
    /* Ensure the OpenGL context is current. */
    glfwMakeContextCurrent(window.NativeHandle());

    /* Load OpenGL function pointers using GLAD. */
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
    {
        std::cout << "RenderSystem: Failed to initialize GLAD." << std::endl;
        return false;
    }

    glEnable(GL_DEPTH_TEST);

    return true;
}
} /* namespace Assisi::Render */
