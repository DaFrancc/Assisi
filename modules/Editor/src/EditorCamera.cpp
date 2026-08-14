/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>
#include "ImGuiQueries.hpp"

#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Editor/ScenePick.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Render/IconPass.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>

#include <cmath>
#include <limits>

namespace Assisi::Editor
{

// ---------------------------------------------------------------------------
// Entity picking + eyedropper
// ---------------------------------------------------------------------------

void EditorApp::HandleEntityPicking()
{
    auto &input = GetInput();
    // A click meant for the gizmo (hovered or dragging) is not a pick: it would
    // reselect whatever is behind the handle.
    if (_actions.IsActionPressed("Select", input) &&
        !input.IsMouseCaptured() && !ImGuiWantsMouse() && !IsUsingGizmo())
    {
        float entityT = 0.f;
        const Assisi::ECS::Entity picked  = PickEntity(input.MousePosition(), entityT);

        // An armed eyedropper consumes the click to fill its EntityRef field; the
        // selection does not move.
        if (_eyedropperArmed)
        {
            ApplyEyedropperPick(picked);
            _eyedropperArmed = false;
            _eyedropperMeta  = nullptr;
            return;
        }

        // An instance's root billboard is clickable too. Selecting an instance is a
        // different gesture from selecting an entity, so it is resolved here rather
        // than folded into PickEntity's return, and the nearer hit wins: a root icon
        // behind a wall must not beat the wall, and a member's mesh in front of the
        // icon must not become unclickable because the icon is there.
        float instanceT  = 0.f;
        const Assisi::ECS::InstanceId instanceId = PickInstance(input.MousePosition(), instanceT);
        if (instanceId.IsValid() && instanceT <= entityT)
        {
            // Exclusive, as every other route to an instance is: it is the whole
            // group, not one more row in a list of entities.
            ClearSelection();
            _selectedInstance = instanceId;
            return;
        }

        // Ctrl *and* Shift both mean "add this one too" out here. A range needs an
        // order to run through, which a list has and the viewport does not, so giving
        // Shift its list meaning would only add a key that does nothing.
        GetEvents().Push(EntitySelectionChangedEvent{picked, ImGuiAdditiveModifier()});
    }
}

void EditorApp::ApplyEyedropperPick(Assisi::ECS::Entity picked)
{
    if (!_eyedropperMeta || !_scene || !_scene->IsAlive(_eyedropperEntity))
        return;

    // Re-resolve rather than caching: the pool can reallocate between arming the
    // field and the click, so the component's address may have moved.
    const void *ptr =
        _eyedropperMeta->getByEntity(_scene, _eyedropperEntity.index, _eyedropperEntity.generation);
    if (!ptr)
        return;

    // Open and commit a one-frame gesture around the raw-offset write below. This is
    // an edit site outside the inspector, so nothing else records it.
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
    auto &input          = GetInput();
    const bool imguiWantsMouse = ImGuiWantsMouse();

    // A running focus animation owns the camera transform: advance the eased blend and
    // skip fly control entirely this frame. Manual look input cancels it — the author
    // is taking over — and either exit re-derives yaw/pitch from the rotation so the
    // fly controller resumes without snapping. The blend is a function of normalized
    // time alone, so it always takes kCameraFocusDuration however far the camera goes.
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
    // Inverts the fly controller's forward-from-(yaw, pitch) mapping, so the
    // controller can pick up from a rotation it did not produce.
    const glm::vec3 forward = glm::normalize(_cameraTransform.rotation * glm::vec3(0.f, 0.f, -1.f));
    _pitch = glm::degrees(std::asin(glm::clamp(forward.y, -1.f, 1.f)));
    _yaw   = glm::degrees(std::atan2(forward.z, forward.x));
}

namespace
{
/// @brief An orientation looking along @p forwardWanted, in the fly camera's basis
///        convention (columns right, up, -forward).
///
/// Identity for a near-zero direction. The reference up is swapped when looking
/// almost straight up or down, where the cross products would be ill-conditioned.
glm::quat LookRotation(glm::vec3 forwardWanted)
{
    const float length = glm::length(forwardWanted);
    if (length < 1e-6f)
    {
        return glm::quat(1.f, 0.f, 0.f, 0.f);
    }
    const glm::vec3 forward = forwardWanted / length;
    glm::vec3 worldUp(0.f, 1.f, 0.f);
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

    // What to frame: a mesh's local sphere through the world matrix, or a small
    // sphere at the origin for a mesh-less entity.
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
    const float dist   = glm::length(toCam);

    // r / sin(halfFovY) is the distance at which the sphere is exactly tangent to the
    // vertical frame; the margin backs off from there so it does not fill the view.
    constexpr float kFrameMargin = 4.f;
    const float halfFovY     = glm::radians(_camera.fovDegrees) * 0.5f;
    const float sinHalf      = glm::sin(halfFovY);
    const float framingDist  = sinHalf > 1e-4f ? (world.radius / sinHalf) * kFrameMargin : world.radius * 3.f;

    // Dolly along the current line of sight, which keeps the viewing angle. If the
    // camera is already inside the sphere, or all but on top of its centre, that
    // direction means nothing — stay put and only re-aim.
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

PickRay EditorApp::BuildPickRay(glm::vec2 mousePos)
{
    PickRay ray;

    RefreshCameraMatrix();
    const glm::mat4 view   = Assisi::Runtime::ViewMatrix(_cameraTransform);
    const auto fbSize = GetWindow().GetFramebufferSize();
    const float w      = static_cast<float>(fbSize.Width);
    const float h      = static_cast<float>(fbSize.Height);
    if (w <= 0.f || h <= 0.f) // minimized/zero-size framebuffer — no valid ray
        return ray;
    const glm::mat4 projection = Assisi::Runtime::ProjectionMatrix(_camera, w / h);

    const float ndcX    = (2.f * mousePos.x / w) - 1.f;
    const float ndcY    = 1.f - (2.f * mousePos.y / h);
    glm::vec4 viewDir = glm::inverse(projection) * glm::vec4(ndcX, ndcY, -1.f, 1.f);
    viewDir.z           = -1.f;
    viewDir.w           = 0.f;

    ray.direction = glm::normalize(glm::vec3(glm::inverse(view) * viewDir));
    ray.origin    = _cameraTransform.position;
    // The camera's world basis, read out of the view matrix's rows. The billboards
    // are built from the same two axes, so a picked icon quad is exactly the drawn
    // one.
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

    const float iconHalf = 0.5f * Assisi::Render::kEntityIconWorldSize;
    Assisi::ECS::InstanceId result;

    // The same quad the renderer draws for an instance root — see
    // EditorApp::SubmitInstanceIcons — so what is clickable is what is visible.
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

    const float iconHalf = 0.5f * Assisi::Render::kEntityIconWorldSize;
    return PickEntityInScene(*_scene, BuildPickRay(mousePos), iconHalf, &MeshPickBounds, tOut);
}

} // namespace Assisi::Editor
