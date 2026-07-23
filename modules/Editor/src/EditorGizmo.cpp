/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/Math/GLM.hpp> // pulls GLMConfig (GLM_ENABLE_EXPERIMENTAL) before the gtx header below
#include <Assisi/Physics/PhysicsComponents.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Hierarchy.hpp>

#include <glm/gtx/matrix_decompose.hpp>

#include <imgui.h> // must precede ImGuizmo.h — it uses ImVec2/ImDrawList/ImU32 unguarded
#include <ImGuizmo.h>

namespace Assisi::Editor
{

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

ImGuizmo::OPERATION ToOperation(EditorApp::GizmoOp op)
{
    switch (op)
    {
    case EditorApp::GizmoOp::Rotate: return ImGuizmo::ROTATE;
    case EditorApp::GizmoOp::Scale:  return ImGuizmo::SCALE;
    case EditorApp::GizmoOp::Translate:
    default:                          return ImGuizmo::TRANSLATE;
    }
}
} // namespace

bool EditorApp::IsUsingGizmo() const
{
    // ImGuizmo tracks the active drag (IsUsing) and hover (IsOver) globally; the
    // picking code checks this to avoid re-selecting when a click lands on the gizmo.
    return ImGuizmo::IsUsing() || ImGuizmo::IsOver();
}

void EditorApp::DrawTransformGizmo()
{
    // Reset ImGuizmo's per-frame state every frame, even with nothing selected, so
    // IsUsing/IsOver read false when the gizmo isn't shown.
    ImGuizmo::BeginFrame();

    // No handles over an inspect-only world: the drag would move an entity whose
    // change could be neither undone nor saved.
    if (_scene == nullptr || !IsEditable() || _selectedEntity == Assisi::ECS::NullEntity ||
        !_scene->IsAlive(_selectedEntity))
    {
        return;
    }
    const Rt::Transform *transform = _scene->Get<Rt::Transform>(_selectedEntity);
    if (transform == nullptr)
    {
        return; // placement-less entity — nothing to manipulate
    }

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

    // A gizmo drag is a Transform edit, so it rides the *same* record-before-write
    // gesture and "Edit Transform" label the inspector uses — no duplicate
    // transaction code. But it force-commits on release (below) so a gizmo drag is
    // always its OWN undo entry, distinct from any inspector Transform edit before
    // or after it.
    //
    // Call RecordBefore every frame the gizmo is drawn, INCLUDING mid-drag: it is
    // idempotent for an already-open gesture (keeps the original pre-drag `before`,
    // just refreshes liveness), so the pre-drag pose still freezes for the drag's
    // duration. Refreshing every frame keeps the gesture alive on its own — the gizmo
    // must not rely on the inspector's per-frame RecordBefore, which runs only while
    // the Transform CollapsingHeader is expanded. Without this, collapsing that header
    // let EndFrameSweep commit-and-drop the (still zero-delta) gesture a frame into the
    // drag, and the `!IsUsing()` guard then blocked a new one — so the drag recorded
    // nothing in the undo history.
    const Assisi::Core::Reflect::ComponentId transformId =
        Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>();
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, transformId, EditLabel("Edit Transform", _selectedEntity),
                              _selectedEntity);

    const bool manipulated = ImGuizmo::Manipulate(&view[0][0], &proj[0][0], operation, mode, &world[0][0],
                                                  nullptr, io.KeyCtrl ? &snap[0] : nullptr);

    const bool nowUsing = ImGuizmo::IsUsing();
    // A held gizmo keeps the shared gesture open until release (mirrors the
    // inspector's active-widget signal).
    if (nowUsing)
        _captureEditingActive = true;

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
                _physics->SetBodyTransform(*body, mutableTransform->position, mutableTransform->rotation);
            }
        }
    }

    // On the drag-release edge, commit the gesture now (before == after drops a
    // click without a drag). Committing here rather than leaving it to the sweep
    // guarantees the gizmo drag is its own transaction — it closes the instant the
    // drag ends, so a Transform edit that follows opens a fresh, separate one.
    if (history != nullptr && !nowUsing && _gizmoWasUsing)
        history->CommitGesture(_selectedEntity, transformId);
    _gizmoWasUsing = nowUsing;
}

} // namespace Assisi::Editor
