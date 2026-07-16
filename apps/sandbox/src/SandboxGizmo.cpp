/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "SandboxApp.hpp"

#include <Assisi/Math/GLM.hpp> // pulls GLMConfig (GLM_ENABLE_EXPERIMENTAL) before the gtx header below
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

#include <glm/gtx/matrix_decompose.hpp>

#include <imgui.h> // must precede ImGuizmo.h — it uses ImVec2/ImDrawList/ImU32 unguarded
#include <ImGuizmo.h>

// ---------------------------------------------------------------------------
// Transform gizmo
// ---------------------------------------------------------------------------
//
// An ImGuizmo manipulator drawn over the 3D viewport for the selected entity.
// Translate / rotate / scale, switched with W / E / R; X toggles world vs. the
// entity's local axes; hold Ctrl to snap. The gizmo edits a world-space matrix,
// which we convert back to the entity's local TRS (so parented entities move
// correctly) and write through GetMut (stamping the change so PropagateTransforms
// re-runs), syncing any RigidBody so a moved physics object stays in step.
//
// Draws into the background draw list so it sits over the scene but under the
// ImGui panels. IsUsingGizmo() lets entity-picking ignore clicks meant for it.

namespace
{
namespace Rt = Assisi::Runtime;

// Snap increments while Ctrl is held: metres for translate, degrees for rotate,
// a unit fraction for scale. Uniform across axes.
constexpr float kTranslateSnap = 0.5f;
constexpr float kRotateSnap    = 15.f;
constexpr float kScaleSnap     = 0.1f;

ImGuizmo::OPERATION ToOperation(SandboxApp::GizmoOp op)
{
    switch (op)
    {
    case SandboxApp::GizmoOp::Rotate: return ImGuizmo::ROTATE;
    case SandboxApp::GizmoOp::Scale:  return ImGuizmo::SCALE;
    case SandboxApp::GizmoOp::Translate:
    default:                          return ImGuizmo::TRANSLATE;
    }
}
} // namespace

bool SandboxApp::IsUsingGizmo() const
{
    // ImGuizmo tracks the active drag (IsUsing) and hover (IsOver) globally; the
    // picking code checks this to avoid re-selecting when a click lands on the gizmo.
    return ImGuizmo::IsUsing() || ImGuizmo::IsOver();
}

void SandboxApp::DrawTransformGizmo()
{
    // Reset ImGuizmo's per-frame state every frame, even with nothing selected, so
    // IsUsing/IsOver read false when the gizmo isn't shown.
    ImGuizmo::BeginFrame();

    const bool haveTarget = _scene != nullptr && _selectedEntity != Assisi::ECS::NullEntity &&
                            _scene->IsAlive(_selectedEntity) &&
                            _scene->Get<Rt::Transform>(_selectedEntity) != nullptr;
    if (!haveTarget)
    {
        // Nothing to manipulate — clear any in-progress gizmo capture state so a
        // selection change mid-drag can't later commit a stale before/after.
        _gizmoManipulating    = false;
        _gizmoWasManipulating = false;
        _gizmoBeforePose.reset();
        return;
    }
    const Rt::Transform *transform = _scene->Get<Rt::Transform>(_selectedEntity);

    // Mode switches, ignored while a text field is capturing keys so typing a name
    // doesn't also retarget the gizmo.
    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantTextInput)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            _gizmoOp = GizmoOp::Translate;
        else if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            _gizmoOp = GizmoOp::Rotate;
        else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            _gizmoOp = GizmoOp::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_X, false))
            _gizmoLocalSpace = !_gizmoLocalSpace;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y);

    // Same view/projection the scene renders with (no manual Y-flip — NVRHI flips
    // the viewport, so on-screen orientation matches ImGuizmo's screen convention).
    const float     aspect = viewport->Size.y > 0.f ? viewport->Size.x / viewport->Size.y : 1.f;
    const glm::mat4 view   = Rt::ViewMatrix(_cameraTransform);
    const glm::mat4 proj   = Rt::ProjectionMatrix(_camera, aspect);

    // The gizmo manipulates a world matrix; a parented entity converts it back to
    // local against the parent's world (roots: identity, world == local).
    glm::mat4 parentWorld(1.f);
    if (const Rt::Parent *parent = _scene->Get<Rt::Parent>(_selectedEntity))
    {
        if (const Rt::Transform *parentTransform = _scene->Get<Rt::Transform>(parent->parent))
        {
            parentWorld = parentTransform->worldMatrix;
        }
    }

    glm::mat4 world = transform->worldMatrix;

    const ImGuizmo::OPERATION operation = ToOperation(_gizmoOp);
    // Scale is inherently along the object's own axes, so ImGuizmo forces LOCAL for
    // it regardless; translate/rotate honour the world/local toggle.
    const ImGuizmo::MODE mode = _gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    const float snapValue = _gizmoOp == GizmoOp::Translate ? kTranslateSnap
                            : _gizmoOp == GizmoOp::Rotate   ? kRotateSnap
                                                            : kScaleSnap;
    const glm::vec3 snap(snapValue);

    // The gizmo owns its Transform edit as its own transaction (NOT the shared
    // record-before-write gesture), so an inspector edit to the same Transform is
    // never mislabelled with the gizmo's mode. Snapshot the pre-drag pose each idle
    // frame; it freezes for the duration of a drag and is the transaction's `before`.
    Sandbox::EditHistory     *history     = ActiveHistory();
    const Assisi::Core::Reflect::ComponentId transformId =
        Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>();

    const bool wasUsing = ImGuizmo::IsUsing(); // last frame's state, until Manipulate runs below
    if (history != nullptr && !wasUsing)
        _gizmoBeforePose = history->SnapshotComponent(_selectedEntity, transformId);

    const bool manipulated = ImGuizmo::Manipulate(&view[0][0], &proj[0][0], operation, mode, &world[0][0],
                                                  nullptr, io.KeyCtrl ? &snap[0] : nullptr);

    const bool nowUsing = ImGuizmo::IsUsing();
    _gizmoManipulating  = nowUsing; // inspector reads this to skip its own Transform capture
    if (nowUsing)
        _captureEditingActive = true;

    // On drag start, drop any Transform gesture the inspector had open on this
    // entity so the two don't both commit for the single change.
    if (history != nullptr && nowUsing && !_gizmoWasManipulating)
        history->AbandonGesture(_selectedEntity, transformId);

    if (manipulated)
    {
        const glm::mat4 local = glm::inverse(parentWorld) * world;

        glm::vec3 scale;
        glm::quat orientation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        if (glm::decompose(local, scale, orientation, translation, skew, perspective))
        {
            Rt::Transform *mutableTransform = _scene->GetMut<Rt::Transform>(_selectedEntity);
            mutableTransform->position = translation;
            mutableTransform->rotation = glm::normalize(orientation);
            mutableTransform->scale    = scale;

            // Keep a physics body in step with the edited pose (scale isn't a body
            // property, so only position/rotation sync — matches the inspector).
            if (const auto *body = _scene->Get<Assisi::Physics::RigidBody>(_selectedEntity))
            {
                _physics.SetBodyTransform(*body, mutableTransform->position, mutableTransform->rotation);
            }
        }
    }

    // On drag release, commit one transaction spanning the whole drag (dropped if the
    // pose didn't actually change — a click without a drag).
    if (history != nullptr && !nowUsing && _gizmoWasManipulating && _gizmoBeforePose.has_value())
    {
        const std::optional<nlohmann::json> after = history->SnapshotComponent(_selectedEntity, transformId);
        if (after.has_value() && *after != *_gizmoBeforePose)
        {
            // A gizmo drag is just a Transform edit — label it the same as an
            // inspector Transform change so the two read consistently in history.
            Sandbox::Transaction txn;
            txn.label           = EditLabel("Edit Transform", _selectedEntity);
            txn.selectionBefore = _selectedEntity;
            txn.selectionAfter  = _selectedEntity;
            txn.cmds.push_back(Sandbox::ComponentDelta{_selectedEntity, transformId, _gizmoBeforePose, after});
            history->Push(std::move(txn));
        }
    }
    _gizmoWasManipulating = nowUsing;
}
