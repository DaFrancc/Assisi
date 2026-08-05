/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/IconPass.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace Assisi::Editor
{

// ---------------------------------------------------------------------------
// Entity picking + eyedropper
// ---------------------------------------------------------------------------

void EditorApp::HandleEntityPicking()
{
    auto &input = GetInput();
    // Skip picking when the click is meant for the transform gizmo (hovered or being
    // dragged) — otherwise clicking a handle would also reselect whatever's behind it.
    if (_actions.IsActionPressed("Select", input) &&
        !input.IsMouseCaptured() && !ImGuiWantsMouse() && !IsUsingGizmo())
    {
        float                     entityT = 0.f;
        const Assisi::ECS::Entity picked  = PickEntity(input.MousePosition(), entityT);

        // An armed eyedropper consumes the click to fill its EntityRef field
        // rather than moving the selection.
        if (_eyedropperArmed)
        {
            ApplyEyedropperPick(picked);
            _eyedropperArmed = false;
            _eyedropperMeta  = nullptr;
            return;
        }

        // An instance's root billboard is clickable too, and selecting an instance is
        // a different gesture from selecting an entity — so it is resolved here
        // rather than folded into PickEntity's return. Nearest wins: a root icon
        // behind a wall should not beat the wall, and a member's mesh in front of the
        // icon should not be unclickable because the icon is there.
        float               instanceT  = 0.f;
        const Assisi::ECS::InstanceId instanceId = PickInstance(input.MousePosition(), instanceT);
        if (instanceId.IsValid() && instanceT <= entityT)
        {
            // Exclusive, like every other way of selecting an instance: it is the
            // whole group, not one more thing in a list of entities.
            ClearSelection();
            _selectedInstance = instanceId;
            return;
        }

        // Ctrl *and* Shift both mean "add this one too" out here. In a list a range
        // is well defined — everything between two rows — but the viewport has no
        // order to draw one through, so binding Shift to anything else would only be
        // a second key that does nothing.
        GetEvents().Push(EntitySelectionChangedEvent{picked, ImGuiAdditiveModifier()});
    }
}

void EditorApp::ApplyEyedropperPick(Assisi::ECS::Entity picked)
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

    // One-frame capture around this raw-offset EntityRef write (an off-inspector
    // edit site that would otherwise slip past the inspector's record-before-write).
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_eyedropperEntity, _eyedropperMeta->id,
                              EditLabel("Assign reference", _eyedropperEntity), _eyedropperEntity);

    auto *field = reinterpret_cast<Assisi::ECS::Entity *>(
        const_cast<char *>(static_cast<const char *>(ptr)) + _eyedropperFieldOffset);
    *field = picked;

    if (history != nullptr)
        history->CommitGesture(_eyedropperEntity, _eyedropperMeta->id);
}

// ---------------------------------------------------------------------------
// Fly camera
// ---------------------------------------------------------------------------

void EditorApp::UpdateCamera(float dt)
{
    auto      &input          = GetInput();
    const bool imguiWantsMouse = ImGuiWantsMouse();

    // A double-click focus animation owns the camera for its fixed duration.
    // Entering look mode cancels it (the author is taking over); otherwise advance
    // the eased blend and skip fly control this frame. The blend is a function of
    // normalized time only, so it always lasts kCameraFocusDuration regardless of
    // how far the camera travels.
    if (_cameraFocusActive)
    {
        if (_actions.IsActionPressed("LookMode", input) && !imguiWantsMouse)
        {
            _cameraFocusActive = false;
            SyncYawPitchFromRotation();
        }
        else
        {
            _cameraFocusElapsed += dt;
            const float u = glm::clamp(_cameraFocusElapsed / kCameraFocusDuration, 0.f, 1.f);
            const float s = u * u * (3.f - (2.f * u)); // smoothstep: ease in and out
            _cameraTransform.position = glm::mix(_cameraFocusStartPos, _cameraFocusEndPos, s);
            _cameraTransform.rotation = glm::slerp(_cameraFocusStartRot, _cameraFocusEndRot, s);
            if (u >= 1.f)
            {
                _cameraFocusActive = false;
                SyncYawPitchFromRotation();
            }
            return;
        }
    }

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

        glm::vec3 move{0.f};
        if (_actions.IsActionDown("MoveForward",  input)) { move += forward; }
        if (_actions.IsActionDown("MoveBackward", input)) { move -= forward; }
        if (_actions.IsActionDown("MoveRight",    input)) { move += right; }
        if (_actions.IsActionDown("MoveLeft",     input)) { move -= right; }
        if (_actions.IsActionDown("MoveUp",       input)) { move.y += 1.f; }
        if (_actions.IsActionDown("MoveDown",     input)) { move.y -= 1.f; }

        if (glm::length(move) > 0.f)
            _cameraTransform.position += glm::normalize(move) * (kMoveSpeed * dt);

        _cameraTransform.rotation = glm::quat_cast(glm::mat3(right, up, -forward));
    }

    if (!imguiWantsMouse)
    {
        const float scroll = input.ScrollDelta();
        if (scroll != 0.f)
            _camera.fovDegrees = glm::clamp(_camera.fovDegrees - (scroll * 5.f), 10.f, 120.f);
    }
}

void EditorApp::RefreshCameraMatrix()
{
    // The editor camera has no parent, so its world matrix is just its local TRS.
    _cameraTransform.worldMatrix = glm::translate(glm::mat4(1.f), _cameraTransform.position) *
                                   glm::mat4_cast(_cameraTransform.rotation) *
                                   glm::scale(glm::mat4(1.f), _cameraTransform.scale);
}

void EditorApp::SyncYawPitchFromRotation()
{
    // Invert the fly controller's forward-from-(yaw,pitch) mapping so it resumes
    // from wherever a focus animation left the camera without snapping back.
    const glm::vec3 forward = glm::normalize(_cameraTransform.rotation * glm::vec3(0.f, 0.f, -1.f));
    _pitch = glm::degrees(std::asin(glm::clamp(forward.y, -1.f, 1.f)));
    _yaw   = glm::degrees(std::atan2(forward.z, forward.x));
}

namespace
{
// Builds an orientation that looks along @p forwardWanted, matching the fly
// camera's basis convention (columns right, up, -forward). Returns identity for a
// degenerate (near-zero) direction, and swaps the reference up when looking almost
// straight up or down so the cross products stay well-conditioned.
glm::quat LookRotation(glm::vec3 forwardWanted)
{
    const float length = glm::length(forwardWanted);
    if (length < 1e-6f)
    {
        return glm::quat(1.f, 0.f, 0.f, 0.f);
    }
    const glm::vec3 forward = forwardWanted / length;
    glm::vec3       worldUp(0.f, 1.f, 0.f);
    if (glm::abs(glm::dot(forward, worldUp)) > 0.999f)
    {
        worldUp = glm::vec3(0.f, 0.f, 1.f);
    }
    const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    const glm::vec3 up    = glm::normalize(glm::cross(right, forward));
    return glm::quat_cast(glm::mat3(right, up, -forward));
}
} // namespace

void EditorApp::FocusCameraOn(Assisi::ECS::Entity entity)
{
    if (_scene == nullptr || !_scene->IsAlive(entity))
    {
        return;
    }
    const Assisi::Runtime::Transform *tc = _scene->Get<Assisi::Runtime::Transform>(entity);
    if (tc == nullptr)
    {
        return; // nothing to frame without a world placement
    }

    // World bounding sphere: a mesh's local sphere mapped through the transform, or
    // a small default around the entity's origin for an empty (mesh-less) object.
    Assisi::Geometry::BoundingSphere world;
    const Assisi::Runtime::MeshRenderer *mrc = _scene->Get<Assisi::Runtime::MeshRenderer>(entity);
    if (mrc != nullptr && mrc->meshBuffer != nullptr && mrc->meshBuffer->LocalBounds().radius > 0.f)
    {
        world = Assisi::Geometry::TransformedBoundingSphere(mrc->meshBuffer->LocalBounds(), tc->worldMatrix);
    }
    else
    {
        world.center           = glm::vec3(tc->worldMatrix[3]);
        const float scaleX     = glm::length(glm::vec3(tc->worldMatrix[0]));
        const float scaleY     = glm::length(glm::vec3(tc->worldMatrix[1]));
        const float scaleZ     = glm::length(glm::vec3(tc->worldMatrix[2]));
        world.radius           = 0.5f * glm::max(scaleX, glm::max(scaleY, scaleZ));
    }
    if (world.radius <= 0.f)
    {
        world.radius = 0.5f;
    }

    RefreshCameraMatrix();
    const glm::vec3 camPos = _cameraTransform.position;
    const glm::vec3 toCam  = camPos - world.center;
    const float     dist   = glm::length(toCam);

    // Distance at which the sphere fills the vertical FOV, with a margin so it
    // isn't edge-to-edge: r / sin(halfFov) puts the sphere tangent to the frame.
    constexpr float kFrameMargin = 4.f;
    const float     halfFovY     = glm::radians(_camera.fovDegrees) * 0.5f;
    const float     sinHalf      = glm::sin(halfFovY);
    const float     framingDist  = sinHalf > 1e-4f ? (world.radius / sinHalf) * kFrameMargin : world.radius * 3.f;

    // First attempt: dolly along the current line of sight to the framing distance,
    // preserving the viewing angle. If the camera is already inside (or all but on
    // top of) the object, that direction is unreliable — give up repositioning and
    // keep the current position, only re-aiming at the centre.
    glm::vec3 targetPos = camPos;
    if (dist >= world.radius && dist > 1e-4f)
    {
        const glm::vec3 dirToCam = toCam / dist;
        targetPos                = world.center + (dirToCam * framingDist);
    }

    _cameraFocusStartPos = camPos;
    _cameraFocusEndPos   = targetPos;
    _cameraFocusStartRot = _cameraTransform.rotation;
    _cameraFocusEndRot   = LookRotation(world.center - targetPos);
    _cameraFocusElapsed  = 0.f;
    _cameraFocusActive   = true;
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

    for (int32_t i = 0; i < 3; ++i)
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

// Ray vs. a camera-facing billboard quad centred at `center`, spanning ±half
// along the (unit) `right` and `up` axes. Matches how IconPass draws entity
// icons, so a meshless entity is clickable only over its drawn billboard rather
// than a whole unit cube.
bool RayBillboardIntersect(glm::vec3 origin, glm::vec3 dir, glm::vec3 center, glm::vec3 right, glm::vec3 up,
                           float half, float &tOut)
{
    const glm::vec3 normal = glm::cross(right, up); // billboard plane faces the camera
    const float     denom  = glm::dot(dir, normal);
    if (std::abs(denom) < 1e-8f)
        return false; // ray parallel to the quad

    const float t = glm::dot(center - origin, normal) / denom;
    if (t < 0.f)
        return false;

    const glm::vec3 hit    = origin + t * dir;
    const glm::vec3 offset = hit - center;
    if (std::abs(glm::dot(offset, right)) <= half && std::abs(glm::dot(offset, up)) <= half)
    {
        tOut = t;
        return true;
    }
    return false;
}

} // namespace

EditorApp::PickRay EditorApp::BuildPickRay(glm::vec2 mousePos)
{
    PickRay ray;

    RefreshCameraMatrix();
    const glm::mat4 view   = Assisi::Runtime::ViewMatrix(_cameraTransform);
    const auto      fbSize = GetWindow().GetFramebufferSize();
    const float     w      = static_cast<float>(fbSize.Width);
    const float     h      = static_cast<float>(fbSize.Height);
    if (w <= 0.f || h <= 0.f) // minimized/zero-size framebuffer — no valid ray
        return ray;
    const glm::mat4 projection = Assisi::Runtime::ProjectionMatrix(_camera, w / h);

    const float ndcX    = (2.f * mousePos.x / w) - 1.f;
    const float ndcY    = 1.f - (2.f * mousePos.y / h);
    glm::vec4   viewDir = glm::inverse(projection) * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    viewDir.z           = -1.f;
    viewDir.w           = 0.f;

    ray.direction = glm::normalize(glm::vec3(glm::inverse(view) * viewDir));
    ray.origin    = _cameraTransform.position;
    // Camera world basis (view rows), matching how the billboards are oriented, so
    // a meshless entity's clickable area is exactly its icon quad.
    ray.cameraRight = glm::vec3(view[0][0], view[1][0], view[2][0]);
    ray.cameraUp    = glm::vec3(view[0][1], view[1][1], view[2][1]);
    ray.valid       = true;
    return ray;
}

Assisi::ECS::InstanceId EditorApp::PickInstance(glm::vec2 mousePos, float &tOut)
{
    tOut = std::numeric_limits<float>::max();
    if (_scene == nullptr || _world == nullptr)
        return {};

    const PickRay ray = BuildPickRay(mousePos);
    if (!ray.valid)
        return {};

    const float             iconHalf = 0.5f * Assisi::Render::kEntityIconWorldSize;
    Assisi::ECS::InstanceId result;

    // The same quad the renderer draws for the instance root, so what is clickable is
    // exactly what is visible (see EditorApp::SubmitInstanceIcons).
    for (const auto &[id, row] : _world->instances.All())
    {
        float t = 0.f;
        if (RayBillboardIntersect(ray.origin, ray.direction, row->transform.position, ray.cameraRight,
                                  ray.cameraUp, iconHalf, t) &&
            t < tOut)
        {
            tOut   = t;
            result = id;
        }
    }
    return result;
}

Assisi::ECS::Entity EditorApp::PickEntity(glm::vec2 mousePos)
{
    float ignored = 0.f;
    return PickEntity(mousePos, ignored);
}

Assisi::ECS::Entity EditorApp::PickEntity(glm::vec2 mousePos, float &tOut)
{
    tOut = std::numeric_limits<float>::max();
    if (!_scene)
        return Assisi::ECS::NullEntity;

    const PickRay ray = BuildPickRay(mousePos);
    if (!ray.valid)
        return Assisi::ECS::NullEntity;

    const glm::vec3 rayOrigin   = ray.origin;
    const glm::vec3 rayDir      = ray.direction;
    const glm::vec3 cameraRight = ray.cameraRight;
    const glm::vec3 cameraUp    = ray.cameraUp;
    const float     iconHalf    = 0.5f * Assisi::Render::kEntityIconWorldSize;

    float               closestT = std::numeric_limits<float>::max();
    Assisi::ECS::Entity result   = Assisi::ECS::NullEntity;

    for (auto [e, tc] : _scene->Query<Assisi::Runtime::Transform>())
    {
        // An entity with a mesh is picked by its (unit-cube) bounds; a placement-
        // only entity is picked by its billboard icon alone, not a big unit cube.
        float      t   = 0.f;
        const bool hit = _scene->Get<Assisi::Runtime::MeshRenderer>(e) != nullptr
                             ? RayOBBIntersect(rayOrigin, rayDir, tc.worldMatrix, t)
                             : RayBillboardIntersect(rayOrigin, rayDir, glm::vec3(tc.worldMatrix[3]), cameraRight,
                                                     cameraUp, iconHalf, t);
        if (hit && t < closestT)
        {
            closestT = t;
            result   = e;
        }
    }

    tOut = closestT;
    return result;
}

} // namespace Assisi::Editor
