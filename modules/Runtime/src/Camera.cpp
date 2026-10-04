/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Camera.hpp>

#include <Assisi/Math/Matrix.hpp>

namespace Assisi::Runtime
{

glm::mat4 ViewMatrix(const glm::mat4 &world)
{
    const glm::vec3 position = Math::TranslationOf(world);
    const glm::vec3 forward  = ForwardDirection(world);
    const glm::vec3 up       = UpDirection(world);
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

glm::vec3 ForwardDirection(const glm::mat4 &world)
{
    return -glm::normalize(Math::ColumnOf(world, Math::MatrixColumn::Back));
}

glm::vec3 RightDirection(const glm::mat4 &world)
{
    return glm::normalize(Math::ColumnOf(world, Math::MatrixColumn::Right));
}

glm::vec3 UpDirection(const glm::mat4 &world)
{
    return glm::normalize(Math::ColumnOf(world, Math::MatrixColumn::Up));
}

glm::mat4 CameraWorldMatrix(const Transform &transform)
{
    return glm::translate(glm::mat4(1.f), transform.position) * glm::mat4_cast(transform.rotation) *
           glm::scale(glm::mat4(1.f), transform.scale);
}

} // namespace Assisi::Runtime
