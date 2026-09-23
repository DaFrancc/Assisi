/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Camera.hpp>

namespace Assisi::Runtime
{

glm::mat4 ViewMatrix(const Transform &transform)
{
    const glm::vec3 position = glm::vec3(transform.worldMatrix[3]);
    const glm::vec3 forward  = -glm::normalize(glm::vec3(transform.worldMatrix[2]));
    const glm::vec3 up       =  glm::normalize(glm::vec3(transform.worldMatrix[1]));
    return glm::lookAt(position, position + forward, up);
}

glm::mat4 ProjectionMatrix(const Camera &camera, float aspectRatio)
{
    return glm::perspective(glm::radians(camera.fovDegrees), aspectRatio, camera.nearZ, camera.farZ);
}

float AspectRatio(int32_t width, int32_t height)
{
    return height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.f;
}

glm::vec3 ForwardDirection(const Transform &transform)
{
    return -glm::normalize(glm::vec3(transform.worldMatrix[2]));
}

glm::vec3 RightDirection(const Transform &transform)
{
    return glm::normalize(glm::vec3(transform.worldMatrix[0]));
}

glm::vec3 UpDirection(const Transform &transform)
{
    return glm::normalize(glm::vec3(transform.worldMatrix[1]));
}

} // namespace Assisi::Runtime