#include <Assisi/Render/Backend/GraphicsBackend.hpp>
#include <Assisi/Render/RenderSystem.hpp>
#include <Assisi/Render/Shader.hpp>
#include <Assisi/Scene/Camera.hpp>
#include <Assisi/Scene/DefaultWorldObjects.hpp>
#include <Assisi/Scene/WorldObject.hpp>
#include <Assisi/Window/WindowContext.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <iostream>

#define WINDOW_HEIGHT 600
#define WINDOW_WIDTH 800

// Triggered when the window is resized
void framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    glViewport(0, 0, width, height);
}

struct InputState
{
    float blendFactor = 0.2f;
    float step = 0.05f;
    // W: (0, +1)
    // S: (0, -1)
    // A: (-1, 0)
    // W: (+1, 0)
    glm::vec2 moveAxis = glm::vec2(0.f);
};

static void KeyCallback(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    /* Ignore unused parameters. */
    (void)scancode;
    (void)mods;

    /* Ignore key repeats so we can use state instead of OS repeat timing. */
    if (action == GLFW_REPEAT)
    {
        return;
    }

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
    {
        glfwSetWindowShouldClose(window, true);
        return;
    }

    if (key == GLFW_KEY_W)
    {
        if (action == GLFW_PRESS)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.y += 1.0f;
        }
        else if (action == GLFW_RELEASE)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.y += -1.0f;
        }
    }

    if (key == GLFW_KEY_A)
    {
        if (action == GLFW_PRESS)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.x += 1.0f;
        }
        else if (action == GLFW_RELEASE)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.x += -1.0f;
        }
    }

    if (key == GLFW_KEY_S)
    {
        if (action == GLFW_PRESS)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.y += -1.0f;
        }
        else if (action == GLFW_RELEASE)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.y += 1.0f;
        }
    }

    if (key == GLFW_KEY_D)
    {
        if (action == GLFW_PRESS)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.x += -1.0f;
        }
        else if (action == GLFW_RELEASE)
        {
            auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));
            state->moveAxis.x += 1.0f;
        }
    }

    if (key == GLFW_KEY_UP && action == GLFW_PRESS)
    {
        /* Read state from the window user pointer. */
        auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));

        if (state->blendFactor >= 1.0f)
            return;

        /* Increase blend factor. */
        state->blendFactor += state->step;

        if (state->blendFactor > 1.0f)
            state->blendFactor = 1.0f;
    }

    if (key == GLFW_KEY_DOWN && action == GLFW_PRESS)
    {
        /* Read state from the window user pointer. */
        auto *state = static_cast<InputState *>(glfwGetWindowUserPointer(window));

        /* Decrease blend factor. */
        state->blendFactor -= state->step;

        if (state->blendFactor < 0.0f)
            state->blendFactor = 0.0f;
    }
}

static void InstallInputCallbacks(GLFWwindow *window, InputState *state)
{
    /* Store state so callbacks can access it. */
    glfwSetWindowUserPointer(window, state);

    /* Install key callback for edge-triggered input. */
    glfwSetKeyCallback(window, KeyCallback);
}

int main()
{
    using Assisi::Render::Backend::GraphicsBackend;
    using Assisi::Render::RenderSystem;

    Assisi::Window::WindowConfiguration configuration{
        .Width = WINDOW_WIDTH, .Height = WINDOW_HEIGHT, .Title = "Assisi", .EnableVSync = true};
    Assisi::Window::WindowContext window(configuration, framebuffer_size_callback);
    if (!window.IsValid())
    {
        return -1;
    }

    if (!RenderSystem::Initialize(GraphicsBackend::OpenGL, window))
    {
        return -1;
    }

    glEnable(GL_DEPTH_TEST);

    InputState input;

    // build and compile our shader zprogram
    // ------------------------------------
    Assisi::Render::Shader ourShader("glsl/Tricolor/tricolor.vs", "glsl/Tricolor/tricolor.fs");

    // set up vertex data (and buffer(s)) and configure vertex attributes
    // ------------------------------------------------------------------
    
    Assisi::Scene::WorldObject cube = Assisi::Scene::CreateDefaultCube();
    // clang-format off
    
    glm::vec3 cubePositions[] = {
        glm::vec3( 0.0f,  0.0f,  0.0f), 
        glm::vec3( 2.0f,  5.0f, -15.0f), 
        glm::vec3(-1.5f, -2.2f, -2.5f),  
        glm::vec3(-3.8f, -2.0f, -12.3f),  
        glm::vec3( 2.4f, -0.4f, -3.5f),  
        glm::vec3(-1.7f,  3.0f, -7.5f),  
        glm::vec3( 1.3f, -2.0f, -2.5f),  
        glm::vec3( 1.5f,  2.0f, -2.5f), 
        glm::vec3( 1.5f,  0.2f, -1.5f), 
        glm::vec3(-1.3f,  1.0f, -1.5f)  
    };

    // clang-format on

    // tell opengl for each sampler to which texture unit it belongs to (only has to be done once)
    // -------------------------------------------------------------------------------------------
    ourShader.Use();
    ourShader.SetInt("texture1", 0);
    ourShader.SetInt("texture2", 1);

    Assisi::Scene::Camera camera;

    glm::mat4 model = glm::mat4(1.0f);
    model = glm::rotate(model, glm::radians(-55.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    glm::mat4 projection;
    projection = glm::perspective(glm::radians(45.0f), 800.0f / 600.0f, 0.1f, 100.0f);

    float deltaTime = 0.f;
    float lastFrameTime = 0.f;

    // render loop
    // -----------
    while (!window.ShouldClose())
    {
        float currentTime = static_cast<float>(glfwGetTime());
        deltaTime = currentTime - lastFrameTime;
        lastFrameTime = currentTime;

        const float cameraSpeed = 5.f; // adjust accordingly
        const glm::vec3 newPosition = camera.WorldPosition() +
                                      camera.ForwardDirection() * input.moveAxis.y * cameraSpeed * deltaTime +
                                      camera.RightDirection() * input.moveAxis.x * cameraSpeed * deltaTime;
        camera.SetWorldPosition(newPosition);
        // render
        // ------

        ourShader.Use();
        ourShader.SetFloat("blend", input.blendFactor);

        ourShader.SetMat4("model", model);

        // camera/view transformation
        glm::mat4 view = glm::mat4(1.0f); // make sure to initialize matrix to identity matrix first
        view = glm::lookAt(camera.WorldPosition(), camera.WorldPosition() + camera.ForwardDirection(),
                           camera.UpDirection());
        ourShader.SetMat4("view", view);
        ourShader.SetMat4("projection", projection);

        // glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
        // -------------------------------------------------------------------------------
        window.PollEvents();
        window.SwapBuffers();
    }

    // optional: de-allocate all resources once they've outlived their purpose:
    // ------------------------------------------------------------------------

    return 0;
}