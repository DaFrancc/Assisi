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
// The ImGuizmo manipulator drawn over the viewport for the current selection.
// W / E / R switch translate / rotate / scale, X cycles the reference frame,
// Ctrl snaps.
//
// The gizmo edits a world-space matrix. We convert it back to the entity's local
// TRS (so parented entities move correctly), write it through GetMut so the change
// is stamped and PropagateTransforms re-runs, then push the new pose to any
// RigidBody — in that order, always.
//
// Drawn into the background draw list: over the scene, under the ImGui panels.
// IsUsingGizmo() lets entity picking ignore clicks meant for it.

namespace
{
namespace Rt = Assisi::Runtime;

// Snap increments while Ctrl is held: metres, degrees, unit fraction. The same
// increment on every axis.
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
    // Hover counts as well as drag: a click landing on a handle must not also
    // re-select whatever is behind it.
    return ImGuizmo::IsUsing() || ImGuizmo::IsOver();
}

namespace
{
/// @brief A TRS as a matrix, composed in the order everything else here uses.
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
        _selectedInstance = {}; // it was deleted while selected
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

    const float aspect = viewport->Size.y > 0.f ? viewport->Size.x / viewport->Size.y : 1.f;
    const glm::mat4 view   = Rt::ViewMatrix(_cameraTransform);
    const glm::mat4 proj   = Rt::ProjectionMatrix(_camera, aspect);

    const Rt::Transform placementBefore = row->transform;
    glm::mat4 world           = TransformMatrix(placementBefore);

    const float snapValue = _gizmoOp == GizmoOp::Translate ? kTranslateSnap
                            : _gizmoOp == GizmoOp::Rotate  ? kRotateSnap
                                                           : kScaleSnap;
    const glm::vec3 snap(snapValue);

    const bool manipulated =
        ImGuizmo::Manipulate(&view[0][0], &proj[0][0], ToOperation(_gizmoOp),
                             _gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &world[0][0], nullptr,
                             io.KeyCtrl ? &snap[0] : nullptr);

    const bool nowUsing = ImGuizmo::IsUsing();

    if (nowUsing)
    {
        BeginInstanceGesture(_selectedInstance);
        _instanceGesture.Hold();
        _captureEditingActive = true;
    }

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
            placement.scale    = scale;
            ApplyInstancePlacement(_selectedInstance, placement);
        }
    }

    // Deliberately no close here. This runs before the Inspector every frame and
    // cannot see that one of its Placement fields is mid-scrub, so closing on "the
    // gizmo is not held" would cut through somebody else's drag, once per frame.
    // SweepInstanceGesture, which runs after every panel, is the only closer.
}

void EditorApp::BeginInstanceGesture(Assisi::ECS::InstanceId instanceId)
{
    if (_scene == nullptr || _world == nullptr)
        return;

    _instanceGesture.Begin(*_scene, _world->instances, ActiveHistory(), instanceId);
}

void EditorApp::SweepInstanceGesture()
{
    // Without a scene or world, the gesture's snapshot names entities that are no
    // longer here — nothing coherent to commit against.
    if (_scene == nullptr || _world == nullptr)
    {
        _instanceGesture.Abandon();
        return;
    }

    _instanceGesture.EndFrame(*_scene, _world->instances, ActiveHistory(), "Move Instance");
}

void EditorApp::ApplyInstancePlacement(Assisi::ECS::InstanceId instanceId, const Rt::Transform &requested)
{
    if (_scene == nullptr || _world == nullptr)
        return;

    const Assisi::Runtime::BlueprintInstance *row = _world->instances.Find(instanceId);
    if (row == nullptr)
        return;

    Rt::Transform placement = requested;
    placement.rotation      = glm::normalize(placement.rotation);
    // One number, not three: an instance may only translate, rotate, and scale
    // *uniformly*. Loading a non-uniform placement hard-fails; averaging here is the
    // half that keeps one from ever being written.
    placement.scale = glm::vec3((placement.scale.x + placement.scale.y + placement.scale.z) / 3.f);
    // Clamped off zero: a zero divides out in the ratio below and takes every
    // member's scale with it, with no record left of what they were.
    if (placement.scale.x <= kMinTypedInstanceScale)
        placement.scale = glm::vec3(kMinTypedInstanceScale);

    // Move the members by the delta rather than re-expanding the instance:
    // re-expansion destroys and recreates entity handles behind undo's back, and
    // this runs every frame of a drag, so it has to stay cheap.
    const glm::mat4 delta = TransformMatrix(placement) * glm::inverse(TransformMatrix(row->transform));
    const glm::quat deltaRotation =
        glm::normalize(placement.rotation * glm::inverse(row->transform.rotation));
    const float scaleRatio =
        row->transform.scale.x != 0.f ? placement.scale.x / row->transform.scale.x : 1.f;

    for (const Assisi::ECS::Entity member : Assisi::Runtime::MembersOf(*_scene, instanceId))
    {
        // Only the members the placement reaches directly. A parented one rides along
        // through its parent, so moving it here would apply the delta twice.
        if (_scene->Has<Rt::Parent>(member))
            continue;

        Rt::Transform *memberTransform = _scene->GetMut<Rt::Transform>(member);
        if (memberTransform == nullptr)
            continue;

        memberTransform->position = glm::vec3(delta * glm::vec4(memberTransform->position, 1.f));
        memberTransform->rotation = glm::normalize(deltaRotation * memberTransform->rotation);
        memberTransform->scale *= scaleRatio;

        // Body last, after the transform is written: it is being kept in step, never
        // consulted.
        if (const auto *body = _scene->Get<Assisi::Physics::RigidBody>(member))
            _physics->SetBodyTransform(*body, memberTransform->position, memberTransform->rotation);
    }

    Assisi::Runtime::BlueprintInstance updated = *row;
    updated.transform                          = placement;
    _world->instances.RestoreAt(instanceId, std::move(updated));
}

void EditorApp::DrawTransformGizmo()
{
    // Every frame, even with nothing selected: otherwise IsUsing/IsOver keep
    // reporting the last state after the gizmo stops being drawn.
    ImGuizmo::BeginFrame();

    // Instance mode: the group moves as one and it is the instance *record* that
    // changes, so no member overrides are recorded. Writing overrides here would
    // pin every member the first time somebody nudged an instance.
    const bool instanceMode = _selectedEntity == Assisi::ECS::NullEntity && _selectedInstance.IsValid();

    // Held is the *only* thing the drawing below reports back, and the release edge
    // is read here rather than at the bottom of it. Every early return in there —
    // instance mode, a dead or non-editable entity, a Transform that went away — is
    // a frame in which the handles are not held, so each of them ends the drag. A
    // release read inside the drawing is unreachable from all four, leaving an open
    // drag to commit a frame later against a different selection.
    const bool held = !instanceMode && DrawTransformGizmoHandles();
    if (!held)
        _gizmoDrag.Release(_scene, ActiveHistory(), Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>());

    if (instanceMode)
        DrawInstanceGizmo();
}

bool EditorApp::DrawTransformGizmoHandles()
{
    // No handles over an inspect-only world: the drag would move an entity whose
    // change can be neither undone nor saved.
    if (_scene == nullptr || _selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity) ||
        !IsEditable(_selectedEntity))
    {
        return false;
    }
    const Rt::Transform *transform = _scene->Get<Rt::Transform>(_selectedEntity);
    if (transform == nullptr)
    {
        return false; // placement-less entity — nothing to manipulate
    }

    // Mode switches, ignored while a text field has the keyboard so typing a name
    // does not also retarget the gizmo.
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
            // Three-way for a member of an instance: World → Local → Instance, the
            // third being the blueprint root's frame, so the handles turn with the
            // instance. A *view* only, never a storage decision — the override is
            // recorded in file space whichever frame is showing.
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

    // The same view/projection the scene renders with. Do not add a Y-flip: NVRHI
    // already flips the viewport, so what is on screen matches ImGuizmo's
    // convention as-is.
    const float aspect = viewport->Size.y > 0.f ? viewport->Size.x / viewport->Size.y : 1.f;
    const glm::mat4 view   = Rt::ViewMatrix(_cameraTransform);
    const glm::mat4 proj   = Rt::ProjectionMatrix(_camera, aspect);

    // The frame the result is converted back through. Identity for a root, where
    // world and local are the same matrix.
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
    // Translate and rotate honour the toggle; scale is along the object's own axes
    // by nature, and ImGuizmo forces LOCAL for it whatever is passed here.
    const ImGuizmo::MODE mode = _gizmoLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

    // ImGuizmo knows only LOCAL and WORLD, so an arbitrary frame is had by folding
    // the instance's placement into the *view* and handing it the member's matrix in
    // instance space. The handles then draw in the instance's axes, and the result
    // is unfolded by the same matrix below, so nothing downstream has to know.
    glm::mat4 gizmoView = view;
    glm::mat4 instanceFrame(1.f);
    bool inInstanceFrame = false;
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
    // gesture and "Edit Transform" label as the inspector rather than duplicating the
    // transaction code. `_gizmoDrag` force-commits it on release — from the caller,
    // out of reach of the early returns above — so that a drag is always its OWN undo
    // entry, separate from any inspector Transform edit before or after it.
    //
    // RecordBefore runs every frame the gizmo is drawn, INCLUDING mid-drag. It is
    // idempotent for an open gesture — the pre-drag `before` is kept, only liveness is
    // refreshed — and that refresh is what keeps the gesture alive here. The
    // inspector's own RecordBefore runs only while its Transform header is expanded,
    // so without this, collapsing that header lets the end-of-frame sweep commit and
    // drop the still-empty gesture a frame into the drag, after which the `!IsUsing()`
    // guard refuses to open a new one and the drag records no undo at all.
    const Assisi::Core::Reflect::ComponentId transformId =
        Assisi::Core::Reflect::ComponentIdOf<Rt::Transform>();
    Assisi::Editor::EditHistory *history = ActiveHistory();
    if (history != nullptr)
        history->RecordBefore(_selectedEntity, transformId, EditLabel("Edit Transform", _selectedEntity),
                              _selectedEntity);

    // The rest of a multi-selection rides the same drag. Each gets its own gesture —
    // the history is keyed by (entity, component), so five entities moving together
    // are five ComponentDeltas — but they all open and close on the same edges, so it
    // is still one gesture to the person dragging.
    //
    // Mid-drag the candidates are the ones the drag grabbed at its press edge, not
    // whatever is selected now: the selection is free to change under an open drag —
    // a click in the entity list does not need the mouse to leave the handle — and a
    // drag that then moved and committed the new selection would be naming entities
    // it never touched.
    //
    // An entity whose parent is also selected is left out: propagation already
    // carries it, and dragging it too would move it twice.
    const std::span<const Assisi::ECS::Entity> riders =
        _gizmoDrag.IsOpen() ? _gizmoDrag.Entities() : std::span<const Assisi::ECS::Entity>(_selection);

    std::vector<Assisi::ECS::Entity> alsoDragged;
    if (riders.size() > 1)
    {
        alsoDragged.reserve(riders.size() - 1);
        for (const Assisi::ECS::Entity entity : riders)
        {
            if (entity == _selectedEntity || !_scene->IsAlive(entity) || !IsEditable(entity))
                continue;
            if (_scene->Get<Rt::Transform>(entity) == nullptr || HasSelectedAncestor(entity))
                continue;
            alsoDragged.push_back(entity);
            if (history != nullptr)
                history->RecordBefore(entity, transformId, EditLabel("Edit Transform", entity), _selectedEntity);
        }
    }

    // The pose the handle started *this frame* at. The rest follow by the change in
    // it, so a rotate turns the group about the handle instead of spinning each one
    // in place.
    const glm::mat4 worldBefore = transform->worldMatrix;

    if (inInstanceFrame)
        world = glm::inverse(instanceFrame) * world;

    const bool manipulated = ImGuizmo::Manipulate(&gizmoView[0][0], &proj[0][0], operation, mode, &world[0][0],
                                                  nullptr, io.KeyCtrl ? &snap[0] : nullptr);

    if (inInstanceFrame)
        world = instanceFrame * world;

    const bool nowUsing = ImGuizmo::IsUsing();
    // A held gizmo keeps the shared gesture open until release, the same signal the
    // inspector raises for an active widget.
    if (nowUsing)
    {
        _captureEditingActive = true;

        // Take the body out of the solver's hands for the drag, as the inspector does
        // for its fields. A body left Dynamic while it is teleported into whatever it
        // overlaps makes the solver resolve that penetration every step: the object
        // creeps and turns on its own under a rotate, and stutters free under a
        // translate. You are placing it, not throwing it at something.
        //
        // Raised *before* the pose is applied below, so the drag's first frame is
        // already frozen. The thaw is at the end of OnImGui, keyed on this request
        // not being raised, so ending the drag by any route — release, deselect, the
        // gizmo vanishing because the world selector moved — restores the body.
        RequestPhysicsFreeze();

        // Opens the drag on the first frame it is held and fixes what it names;
        // every frame after this one only re-asserts the hold.
        _gizmoDrag.Hold(_selectedEntity, alsoDragged);
    }

    if (manipulated)
    {
        // The handle's own entity takes the matrix ImGuizmo produced, not a delta, so
        // it lands exactly under the handle even after a long drag has accumulated
        // rounding.
        ApplyGizmoWorldMatrix(_selectedEntity, parentWorld, world);

        // The rest follow by the same world-space change, applied to their current
        // pose rather than to a start-of-drag snapshot: this runs every frame, and
        // `worldBefore` is measured from the start of *this* frame.
        if (!alsoDragged.empty())
        {
            const glm::mat4 delta = world * glm::inverse(worldBefore);
            for (const Assisi::ECS::Entity entity : alsoDragged)
            {
                const Rt::Transform *entityTransform = _scene->Get<Rt::Transform>(entity);
                if (entityTransform == nullptr)
                    continue;

                glm::mat4 entityParentWorld(1.f);
                if (const Rt::Parent *parent = _scene->Get<Rt::Parent>(entity))
                {
                    if (const Rt::Transform *parentTransform = _scene->Get<Rt::Transform>(parent->parent))
                        entityParentWorld = parentTransform->worldMatrix;
                }
                ApplyGizmoWorldMatrix(entity, entityParentWorld, delta * entityTransform->worldMatrix);
            }
        }
    }

    // The release edge belongs to the caller, which reads it whether or not this
    // function ran at all. See DrawTransformGizmo.
    return nowUsing;
}

void EditorApp::ApplyGizmoWorldMatrix(Assisi::ECS::Entity entity, const glm::mat4 &parentWorld,
                                      const glm::mat4 &world)
{
    // Back to local against the parent's world; `parentWorld` is identity for a
    // root, where local and world are the same matrix.
    const glm::mat4 local = glm::inverse(parentWorld) * world;

    glm::vec3 scale;
    glm::quat orientation;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;
    if (!glm::decompose(local, scale, orientation, translation, skew, perspective))
        return;

    Rt::Transform *mutableTransform = _scene->GetMut<Rt::Transform>(entity);
    if (mutableTransform == nullptr)
        return;
    mutableTransform->position = translation;
    mutableTransform->rotation = glm::normalize(orientation);
    mutableTransform->scale    = scale;

    // Body last, after the pose is written. Position and rotation only — scale is not
    // a body property, and the inspector syncs the same two.
    if (const auto *body = _scene->Get<Assisi::Physics::RigidBody>(entity))
    {
        _physics->SetBodyTransform(*body, mutableTransform->position, mutableTransform->rotation);
    }
}

} // namespace Assisi::Editor
