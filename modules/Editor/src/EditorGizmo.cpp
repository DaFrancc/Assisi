/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Editor/EditorApp.hpp>

#include <Assisi/ECS/BlueprintMember.hpp>
#include <Assisi/Runtime/Blueprint.hpp>

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

namespace
{
/// A TRS as a matrix, in the order everything else in the engine composes it.
glm::mat4 TransformMatrix(const Rt::Transform &transform)
{
    return glm::translate(glm::mat4(1.f), transform.position) * glm::mat4_cast(transform.rotation) *
           glm::scale(glm::mat4(1.f), transform.scale);
}
} // namespace

void EditorApp::DrawInstanceGizmo()
{
    if (_scene == nullptr || _world == nullptr || !IsEditable())
        return;

    const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(_selectedInstance);
    if (row == nullptr)
    {
        _selectedInstance = 0; // the instance went away while it was selected
        return;
    }

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

    const float     aspect = viewport->Size.y > 0.f ? viewport->Size.x / viewport->Size.y : 1.f;
    const glm::mat4 view   = Rt::ViewMatrix(_cameraTransform);
    const glm::mat4 proj   = Rt::ProjectionMatrix(_camera, aspect);

    const Rt::Transform placementBefore = row->transform;
    glm::mat4           world           = TransformMatrix(placementBefore);

    const float snapValue = _gizmoOp == GizmoOp::Translate ? kTranslateSnap
                            : _gizmoOp == GizmoOp::Rotate  ? kRotateSnap
                                                           : kScaleSnap;
    const glm::vec3 snap(snapValue);

    const bool manipulated =
        ImGuizmo::Manipulate(&view[0][0], &proj[0][0], ToOperation(_gizmoOp),
                             _gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &world[0][0], nullptr,
                             io.KeyCtrl ? &snap[0] : nullptr);

    const bool nowUsing = ImGuizmo::IsUsing();

    // Press edge: snapshot the record and every member's pose, because the undo
    // entry has to take both back together and neither is reconstructible after
    // the fact.
    if (nowUsing && _instanceDragId != _selectedInstance)
    {
        _instanceDragId  = _selectedInstance;
        _instanceDragRow = *row;
        _instanceDragPoses.clear();

        if (Assisi::Editor::EditHistory *history = ActiveHistory())
        {
            const auto transformId = Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>();
            for (const Assisi::ECS::Entity member : Assisi::Runtime::MembersOf(*_scene, _selectedInstance))
            {
                if (std::optional<nlohmann::json> pose = history->CaptureComponent(member, transformId))
                    _instanceDragPoses.emplace_back(member, std::move(*pose));
            }
        }
    }

    if (nowUsing)
        _captureEditingActive = true;

    if (manipulated)
    {
        glm::vec3 scale;
        glm::quat orientation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        if (glm::decompose(world, scale, orientation, translation, skew, perspective))
        {
            Rt::Transform placement;
            placement.position = translation;
            placement.rotation = glm::normalize(orientation);
            // One number, not three: an instance may only translate, rotate, or
            // scale *uniformly*, and the editor is where that is enforced first so
            // a violation cannot be authored at all (§3). The load hard-fails on
            // one anyway; this is the half that keeps it from ever being written.
            placement.scale = glm::vec3((scale.x + scale.y + scale.z) / 3.f);

            // Move the members by the delta rather than re-expanding: re-expansion
            // would destroy and recreate handles behind undo's back, and dragging
            // is the one gesture that must stay cheap.
            const glm::mat4 delta = TransformMatrix(placement) * glm::inverse(TransformMatrix(row->transform));
            const glm::quat deltaRotation =
                glm::normalize(placement.rotation * glm::inverse(row->transform.rotation));

            for (const Assisi::ECS::Entity member : Assisi::Runtime::MembersOf(*_scene, _selectedInstance))
            {
                // Only the members the placement reaches directly; a parented one
                // rides along through its parent, and moving it too would apply the
                // delta twice.
                if (_scene->Has<Rt::Parent>(member))
                    continue;

                Rt::Transform *memberTransform = _scene->GetMut<Rt::Transform>(member);
                if (memberTransform == nullptr)
                    continue;

                memberTransform->position = glm::vec3(delta * glm::vec4(memberTransform->position, 1.f));
                memberTransform->rotation = glm::normalize(deltaRotation * memberTransform->rotation);
                memberTransform->scale *= placement.scale.x / row->transform.scale.x;

                if (const auto *body = _scene->Get<Assisi::Physics::RigidBody>(member))
                    _physics->SetBodyTransform(*body, memberTransform->position, memberTransform->rotation);
            }

            Assisi::Runtime::BlueprintInstance updated = *row;
            updated.transform                          = placement;
            _world->instances.RestoreAt(_selectedInstance, std::move(updated));
        }
    }

    // Release edge: one transaction carrying the record and every pose it moved.
    if (!nowUsing && _instanceDragId != 0)
    {
        if (Assisi::Editor::EditHistory *history = ActiveHistory())
        {
            const Assisi::Runtime::BlueprintInstance *now = _world->instances.Find(_instanceDragId);
            if (now != nullptr)
            {
                Assisi::Editor::Transaction txn;
                txn.label = "Move Instance";
                txn.cmds.push_back(Assisi::Editor::InstanceDelta{
                    .instanceId = _instanceDragId, .before = _instanceDragRow, .after = *now});

                const auto transformId = Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>();
                for (const auto &[member, before] : _instanceDragPoses)
                {
                    if (!_scene->IsAlive(member))
                        continue;
                    std::optional<nlohmann::json> after = history->CaptureComponent(member, transformId);
                    if (after != std::optional<nlohmann::json>{before})
                        txn.cmds.push_back(Assisi::Editor::ComponentDelta{member, transformId, before, after});
                }

                // Only if something actually moved — a click without a drag is not
                // an edit, the same rule the gesture machinery applies elsewhere.
                if (txn.cmds.size() > 1)
                    history->Push(std::move(txn));
            }
        }

        _instanceDragId = 0;
        _instanceDragPoses.clear();
    }
}

void EditorApp::DrawTransformGizmo()
{
    // Reset ImGuizmo's per-frame state every frame, even with nothing selected, so
    // IsUsing/IsOver read false when the gizmo isn't shown.
    ImGuizmo::BeginFrame();

    // Instance mode: the whole group moves as one and the *record* is what changes,
    // recording no member overrides. Getting this wrong pins all five members the
    // first time somebody nudges a car (docs/blueprint-system-concept.md §3).
    if (_selectedEntity == Assisi::ECS::NullEntity && _selectedInstance != 0)
    {
        DrawInstanceGizmo();
        return;
    }

    // No handles over an inspect-only world: the drag would move an entity whose
    // change could be neither undone nor saved.
    if (_scene == nullptr || _selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity) ||
        !IsEditable(_selectedEntity))
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
        {
            // Three-way for a member of an instance: World → Local → Instance. The
            // third frame is the blueprint root's, so the handles rotate with the
            // car — a *view*, never a storage decision, since the override is
            // recorded in file space either way.
            const bool isMember = _scene->Has<Assisi::ECS::BlueprintMember>(_selectedEntity);
            if (!isMember)
            {
                _gizmoLocalSpace    = !_gizmoLocalSpace;
                _gizmoInstanceSpace = false;
            }
            else if (!_gizmoLocalSpace && !_gizmoInstanceSpace)
                _gizmoLocalSpace = true;
            else if (_gizmoLocalSpace)
            {
                _gizmoLocalSpace    = false;
                _gizmoInstanceSpace = true;
            }
            else
                _gizmoInstanceSpace = false;
        }
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

    // ImGuizmo only knows LOCAL and WORLD, so an arbitrary frame is had by folding
    // the instance's placement into the *view* and handing it the member's matrix
    // in instance space (§3). The result is drawn in the car's axes and decomposes
    // back through the same fold, so nothing downstream has to know.
    glm::mat4 gizmoView = view;
    glm::mat4 instanceFrame(1.f);
    bool      inInstanceFrame = false;
    if (_gizmoInstanceSpace && _world != nullptr)
    {
        if (const auto *tag = _scene->Get<Assisi::ECS::BlueprintMember>(_selectedEntity))
        {
            if (const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(tag->instanceId))
            {
                instanceFrame   = TransformMatrix(row->transform);
                gizmoView       = view * instanceFrame;
                inInstanceFrame = true;
            }
        }
    }

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

    if (inInstanceFrame)
        world = glm::inverse(instanceFrame) * world;

    const bool manipulated = ImGuizmo::Manipulate(&gizmoView[0][0], &proj[0][0], operation, mode, &world[0][0],
                                                  nullptr, io.KeyCtrl ? &snap[0] : nullptr);

    if (inInstanceFrame)
        world = instanceFrame * world;

    const bool nowUsing = ImGuizmo::IsUsing();
    // A held gizmo keeps the shared gesture open until release (mirrors the
    // inspector's active-widget signal).
    if (nowUsing)
    {
        _captureEditingActive = true;

        // Take the body out of the solver's hands for the duration of the drag,
        // exactly as the inspector does for its fields. Without this the body is
        // still Dynamic while being teleported into whatever it overlaps, so the
        // solver spends every step resolving that penetration — the object creeps
        // and rotates on its own during a rotate, and stutters as it squeezes free
        // during a translate. You are placing it, not throwing it at something.
        //
        // Raised *before* the pose is applied below, so the very first frame of the
        // drag is already frozen. Released at the end of OnImGui, so ending the drag
        // by any route (release, deselect, the gizmo vanishing because the world
        // selector moved) still restores the body.
        RequestPhysicsFreeze();
    }

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
