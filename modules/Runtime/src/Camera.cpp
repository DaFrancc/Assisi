#include <Assisi/Runtime/Camera.hpp>

namespace Assisi::Runtime
{

glm::mat4 ViewMatrix(const TransformComponent &transform)
{
    const glm::vec3 forward = transform.rotation * glm::vec3(0.f, 0.f, -1.f);
    const glm::vec3 up      = transform.rotation * glm::vec3(0.f, 1.f,  0.f);
    return glm::lookAt(transform.position, transform.position + forward, up);
}

glm::mat4 ProjectionMatrix(const CameraComponent &camera, float aspectRatio)
{
    return glm::perspective(glm::radians(camera.fovDegrees), aspectRatio, camera.nearZ, camera.farZ);
}

glm::vec3 ForwardDirection(const TransformComponent &transform)
{
    return transform.rotation * glm::vec3(0.f, 0.f, -1.f);
}

glm::vec3 RightDirection(const TransformComponent &transform)
{
    return transform.rotation * glm::vec3(1.f, 0.f, 0.f);
}

glm::vec3 UpDirection(const TransformComponent &transform)
{
    return transform.rotation * glm::vec3(0.f, 1.f, 0.f);
}

} // namespace Assisi::Runtime
