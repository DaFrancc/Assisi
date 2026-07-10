/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"
#include "SandboxImGui.hpp"

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Entity picking + eyedropper
// ---------------------------------------------------------------------------

void SandboxApp::HandleEntityPicking()
{
    auto &input = GetInput();
    if (_actions.IsActionPressed("Select", input) &&
        !input.IsMouseCaptured() && !ImGuiWantsMouse())
    {
        const Assisi::ECS::Entity picked = PickEntity(input.MousePosition());

        // An armed eyedropper consumes the click to fill its EntityRef field
        // rather than moving the selection.
        if (_eyedropperArmed)
        {
            ApplyEyedropperPick(picked);
            _eyedropperArmed = false;
            _eyedropperMeta  = nullptr;
        }
        else
        {
            GetEvents().Push(EntitySelectionChangedEvent{picked});
        }
    }
}

void SandboxApp::ApplyEyedropperPick(Assisi::ECS::Entity picked)
{
    if (!_eyedropperMeta || !_scene || !_scene->IsAlive(_eyedropperEntity))
        return;

    // The component pointer may have moved since the field was armed (the pool
    // can reallocate), so re-resolve it now and write straight into the
    // reflected offset.
    const void *ptr =
        _eyedropperMeta->getByEntity(_scene, _eyedropperEntity.index, _eyedropperEntity.generation);
    if (!ptr)
        return;

    auto *field = reinterpret_cast<Assisi::ECS::Entity *>(
        const_cast<char *>(static_cast<const char *>(ptr)) + _eyedropperFieldOffset);
    *field = picked;
}

// ---------------------------------------------------------------------------
// Fly camera
// ---------------------------------------------------------------------------

void SandboxApp::UpdateCamera(float dt)
{
    auto      &input          = GetInput();
    const bool imguiWantsMouse = ImGuiWantsMouse();

    if (_actions.IsActionPressed("LookMode", input) && !imguiWantsMouse)
        input.SetMouseCaptured(true);
    if (_actions.IsActionReleased("LookMode", input))
        input.SetMouseCaptured(false);

    if (input.IsMouseCaptured())
    {
        const glm::vec2 delta = input.MouseDelta();
        _yaw   += delta.x * kMouseSensitivity;
        _pitch -= delta.y * kMouseSensitivity;
        _pitch  = glm::clamp(_pitch, -89.f, 89.f);

        const glm::vec3 forward = {
            glm::cos(glm::radians(_pitch)) * glm::cos(glm::radians(_yaw)),
            glm::sin(glm::radians(_pitch)),
            glm::cos(glm::radians(_pitch)) * glm::sin(glm::radians(_yaw))};

        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.f, 1.f, 0.f}));
        const glm::vec3 up    = glm::normalize(glm::cross(right, forward));

        auto *camTransform =
            _cameraScene.Get<Assisi::Runtime::Transform>(_cameraEntity);

        glm::vec3 move{0.f};
        if (_actions.IsActionDown("MoveForward",  input)) { move += forward; }
        if (_actions.IsActionDown("MoveBackward", input)) { move -= forward; }
        if (_actions.IsActionDown("MoveRight",    input)) { move += right; }
        if (_actions.IsActionDown("MoveLeft",     input)) { move -= right; }
        if (_actions.IsActionDown("MoveUp",       input)) { move.y += 1.f; }
        if (_actions.IsActionDown("MoveDown",     input)) { move.y -= 1.f; }

        if (glm::length(move) > 0.f)
            camTransform->position += glm::normalize(move) * (kMoveSpeed * dt);

        camTransform->rotation = glm::quat_cast(glm::mat3(right, up, -forward));
    }

    if (!imguiWantsMouse)
    {
        const float scroll = input.ScrollDelta();
        if (scroll != 0.f)
        {
            auto *cam       = _cameraScene.Get<Assisi::Runtime::Camera>(_cameraEntity);
            cam->fovDegrees = glm::clamp(cam->fovDegrees - (scroll * 5.f), 10.f, 120.f);
        }
    }
}

// ---------------------------------------------------------------------------
// Ray picking
// ---------------------------------------------------------------------------

namespace
{

bool RayOBBIntersect(glm::vec3 origin, glm::vec3 dir, const glm::mat4 &model, float &tOut)
{
    const glm::mat4 inv   = glm::inverse(model);
    const glm::vec3 lOrig = glm::vec3(inv * glm::vec4(origin, 1.f));
    const glm::vec3 lDir  = glm::vec3(inv * glm::vec4(dir, 0.f));

    float tMin = -std::numeric_limits<float>::max();
    float tMax =  std::numeric_limits<float>::max();

    for (int i = 0; i < 3; ++i)
    {
        if (std::abs(lDir[i]) < 1e-8f)
        {
            if (lOrig[i] < -0.5f || lOrig[i] > 0.5f)
                return false;
        }
        else
        {
            float t1 = (-0.5f - lOrig[i]) / lDir[i];
            float t2 = ( 0.5f - lOrig[i]) / lDir[i];
            if (t1 > t2)
                std::swap(t1, t2);
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax)
                return false;
        }
    }

    if (tMax < 0.f)
        return false;

    tOut = tMin > 0.f ? tMin : tMax;
    return true;
}

} // namespace

Assisi::ECS::Entity SandboxApp::PickEntity(glm::vec2 mousePos)
{
    if (!_scene)
        return Assisi::ECS::NullEntity;

    const auto     *camTransform = _cameraScene.Get<Assisi::Runtime::Transform>(_cameraEntity);
    const auto     *cam          = _cameraScene.Get<Assisi::Runtime::Camera>(_cameraEntity);
    const glm::mat4 view         = Assisi::Runtime::ViewMatrix(*camTransform);
    const auto      fbSize       = GetWindow().GetFramebufferSize();
    const float     w            = static_cast<float>(fbSize.Width);
    const float     h            = static_cast<float>(fbSize.Height);
    if (w <= 0.f || h <= 0.f) // minimized/zero-size framebuffer — no valid ray
        return Assisi::ECS::NullEntity;
    const glm::mat4 projection   = Assisi::Runtime::ProjectionMatrix(*cam, w / h);

    const float     ndcX    = (2.f * mousePos.x / w) - 1.f;
    const float     ndcY    = 1.f - (2.f * mousePos.y / h);
    glm::vec4       viewDir = glm::inverse(projection) * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    viewDir.z = -1.f;
    viewDir.w =  0.f;
    const glm::vec3 rayDir    = glm::normalize(glm::vec3(glm::inverse(view) * viewDir));
    const glm::vec3 rayOrigin = camTransform->position;

    float               closestT = std::numeric_limits<float>::max();
    Assisi::ECS::Entity result   = Assisi::ECS::NullEntity;

    for (auto [e, tc] : _scene->Query<Assisi::Runtime::Transform>())
    {
        float t = 0.f;
        if (RayOBBIntersect(rayOrigin, rayDir, tc.worldMatrix, t) && t < closestT)
        {
            closestT = t;
            result   = e;
        }
    }

    return result;
}
