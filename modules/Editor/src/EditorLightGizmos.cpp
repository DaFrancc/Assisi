/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorLightGizmos.cpp
/// @brief What a light looks like while it is being placed.
///
/// A light has no geometry, so the viewport shows only its billboard — which
/// says where it is and nothing about what it reaches. Its radius, its cone and
/// its aim are authored by typing numbers and judging the lit result, and that
/// loop is slow in proportion to how wrong the first guess was.
///
/// Each shape is the light's own parameters drawn literally: a point light's
/// sphere is its `radius`, a spot's outer cone is its `outerAngle` swept to that
/// same radius, and a directional light's arrow is the direction it travels. So
/// what is on screen is the field in the inspector, and a shape that looks wrong
/// is a number that is wrong.

#include <Assisi/Editor/EditorApp.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include <Assisi/ECS/Transform.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/LinePass.hpp>
#include <Assisi/Runtime/Camera.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/LightingSystem.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <imgui.h> // must precede ImGuizmo.h — it uses ImVec2/ImDrawList/ImU32 unguarded
#include <ImGuizmo.h>

namespace Assisi::Editor
{
namespace
{
using Assisi::Render::LineVertex;
namespace Rt = Assisi::Runtime;

// The unselected colour is the light's own, dimmed: a warm light draws warm and
// a cold one cold, so a scene of lights reads as its own lighting plan rather
// than as a mass of identical wireframes. The alpha is what keeps a dense scene
// legible — twenty overlapping spheres at full strength are a solid.
constexpr float kUnselectedAlpha = 0.35f;

// Selected lights borrow the same two constants the mesh silhouette and the
// collider wireframes use, so "selected" and "this is the one the inspector is
// talking about" are said in one vocabulary across every overlay.
constexpr glm::vec4 kSelectedColor{Rt::kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{Rt::kActiveSelectionOutline, 1.0f};

/// @brief How long a directional light's arrow is drawn, in world units.
///
/// A directional light has no scale of its own — it is infinitely far away and
/// its reach is the whole level — so this is a readable size rather than a
/// measurement of anything. Nothing downstream reads it.
constexpr float kSunArrowLength = 2.0f;

/// @brief Degrees the sun's aim snaps to while ctrl is held, matching the
/// transform gizmo's own rotate snap so one modifier means one thing.
constexpr float kSunRotateSnapDegrees = 15.0f;

/// @brief The colour an unselected light draws in: its own, at a fraction of the
/// strength, and never black.
glm::vec4 UnselectedColor(const glm::vec3 &lightColor)
{
    // Normalised rather than used raw: an intensity of ten would clip to white
    // and one of a hundredth would vanish, and neither says anything about where
    // the light reaches. The floor keeps a pure-black light visible, which is a
    // light someone is probably trying to find out about.
    const float brightest = std::max({lightColor.r, lightColor.g, lightColor.b, 1e-3f});
    return glm::vec4(glm::max(lightColor / brightest, glm::vec3(0.15f)), kUnselectedAlpha);
}

/// @brief Where an entity sits, or the world origin when it has no Transform.
///
/// A directional light commonly has none — it has no position to speak of — and
/// the alternative to a fallback is a gizmo drawn nowhere.
glm::vec3 EntityPosition(Assisi::ECS::Scene &scene, Assisi::ECS::Entity entity)
{
    if (const Rt::Transform *transform = scene.Get<Rt::Transform>(entity))
    {
        return glm::vec3(transform->worldMatrix[3]);
    }
    return glm::vec3(0.f);
}
} // namespace

void EditorApp::SubmitLightGizmos()
{
    if (_scene == nullptr)
    {
        return;
    }

    _lightLinesDepthTested.clear();
    _lightLinesOnTop.clear();

    // Which colour and which batch a light draws in. The selection goes on top so
    // the light being edited is never hidden behind the geometry it lights, which
    // is exactly where a light usually is.
    const auto styleFor = [&](Assisi::ECS::Entity entity, const glm::vec3 &lightColor)
                          {
                              const bool selected = IsSelected(entity);
                              const bool active = entity == _selectedEntity;
                              struct Style
                              {
                                  std::vector<LineVertex> *batch;
                                  glm::vec4 color;
                                  bool detailed;
                              };
                              if (!selected)
                              {
                                  return Style{&_lightLinesDepthTested, UnselectedColor(lightColor), false};
                              }
                              return Style{&_lightLinesOnTop, active ? kActiveSelectedColor : kSelectedColor, true};
                          };

    for (auto [entity, transform, light] : _scene->Query<Rt::Transform, Rt::PointLight>())
    {
        const auto style = styleFor(entity, glm::vec3(light.color));
        // Translation only. A point light is a sphere however its entity is
        // scaled or turned, and inheriting the rotation would draw an ellipsoid
        // that claims a reach the light does not have.
        const glm::mat4 model = glm::translate(glm::mat4(1.f), glm::vec3(transform.worldMatrix[3]));
        AddSphereWireframe(*style.batch, model, style.color, light.radius);
    }

    for (auto [entity, transform, light] : _scene->Query<Rt::Transform, Rt::SpotLight>())
    {
        const auto style = styleFor(entity, light.color);
        // The aim the renderer uses, not the authored local one: a spot mounted
        // on a parent points where the parent faces, and a cone drawn along the
        // unrotated field would disagree with the light it describes.
        const glm::vec3 aim = Rt::LightingSystem::WorldSpotDirection(transform.worldMatrix, light.direction);
        const glm::mat4 model =
            glm::translate(glm::mat4(1.f), glm::vec3(transform.worldMatrix[3])) * AimAlong(aim);

        AddConeWireframe(*style.batch, model, style.color, light.outerAngle, light.radius, style.detailed);
        if (style.detailed)
        {
            // The inner cone only while selected. It is the full-brightness core
            // and the outer is where the falloff ends, so seeing both is how the
            // softness of the edge is judged — but two cones per light is twice
            // the clutter in a scene that is not being edited.
            AddConeWireframe(*style.batch, model, style.color, light.innerAngle, light.radius, false);
        }
    }

    for (auto [entity, light] : _scene->Query<Rt::DirectionalLight>())
    {
        const auto style = styleFor(entity, glm::vec3(Rt::AuthoredSunColor(light)));
        const glm::mat4 model =
            glm::translate(glm::mat4(1.f), EntityPosition(*_scene, entity)) * AimAlong(light.direction);
        AddArrowWireframe(*style.batch, model, style.color, kSunArrowLength);
    }

    _sceneRenderer.SubmitOverlayLines(_lightLinesDepthTested, /*onTop=*/ false);
    _sceneRenderer.SubmitOverlayLines(_lightLinesOnTop, /*onTop=*/ true);
}

bool EditorApp::DrawDirectionalLightGizmo()
{
    if (_scene == nullptr || _selectedEntity == Assisi::ECS::NullEntity || !_scene->IsAlive(_selectedEntity) ||
        !IsEditable(_selectedEntity))
    {
        return false;
    }
    Rt::DirectionalLight *light = _scene->Get<Rt::DirectionalLight>(_selectedEntity);
    if (light == nullptr)
    {
        return false;
    }

    // Only while the toolbar is on Rotate, and this is not cosmetic. ImGuizmo
    // keeps ONE interaction state per frame, so two Manipulate calls in a frame
    // fight over it — and taking the frame only when *held* meant exactly that
    // on every frame a sun was selected and not being dragged: this one drew,
    // reported no hold, and the transform gizmo then drew its own over the top.
    //
    // Gating on the operation gives one call either way. Rotate aims the light,
    // which is the edit it has; translate and scale fall through to the entity's
    // Transform, which is the only thing they could act on.
    if (_gizmoOp != GizmoOp::Rotate)
    {
        return false;
    }

    ImGuiIO &io = ImGui::GetIO();
    const float aspect = io.DisplaySize.y > 0.f ? io.DisplaySize.x / io.DisplaySize.y : 1.f;
    const glm::mat4 view = Rt::ViewMatrix(_cameraTransform);
    const glm::mat4 proj = Rt::ProjectionMatrix(_camera, aspect);

    // Where the arrow is, which is where its handles are. A directional light
    // needs no Transform and often has none, so the origin is the fallback rather
    // than an error: a gizmo with nowhere to be is one nobody can grab.
    const glm::vec3 pivot = EntityPosition(*_scene, _selectedEntity);

    // The arrow's own pose, as a matrix ImGuizmo can turn. Rotation only — a
    // directional light has no scale and its position is a convenience, so a
    // handle that moved either would be offering an edit with no effect.
    glm::mat4 arrow = glm::translate(glm::mat4(1.f), pivot) * AimAlong(light->direction);

    const glm::vec3 snap(kSunRotateSnapDegrees);

    Assisi::Editor::EditHistory *history = ActiveHistory();
    const Assisi::Core::Reflect::ComponentId lightId =
        Assisi::Core::Reflect::ComponentIdOf<Rt::DirectionalLight>();
    // Every frame the handles are drawn, mid-drag included. RecordBefore is
    // idempotent for an open gesture — the pre-drag `before` is kept and only
    // liveness is refreshed — and that refresh is what stops the end-of-frame
    // sweep committing the drag out from under itself.
    if (history != nullptr)
    {
        history->RecordBefore(_selectedEntity, lightId, EditLabel("Aim Light", _selectedEntity), _selectedEntity);
    }

    const bool manipulated = ImGuizmo::Manipulate(&view[0][0], &proj[0][0], ImGuizmo::ROTATE, ImGuizmo::WORLD,
                                                  &arrow[0][0], nullptr, io.KeyCtrl ? &snap[0] : nullptr);
    const bool held = ImGuizmo::IsUsing();

    if (manipulated)
    {
        // The direction is the shape's forward carried through the rotation the
        // handles just made — read back off the matrix rather than accumulated,
        // so a drag cannot drift from what is drawn.
        const glm::vec3 turned = glm::vec3(arrow * glm::vec4(0.f, -1.f, 0.f, 0.f));
        const float lengthSq = glm::dot(turned, turned);
        // A degenerate result would put a NaN in the light's direction, and a NaN
        // there reaches the cascade fit and takes every shadowed pixel with it.
        if (std::isfinite(lengthSq) && lengthSq > 0.f)
        {
            light->direction = turned / std::sqrt(lengthSq);
            _scene->MarkChanged(_selectedEntity, lightId);
        }
    }

    // Held is all this reports. The release edge is driven from the caller, for
    // the reason GizmoDrag exists: every early return above is a frame in which
    // the handles are not held, and a commit written down here is unreachable
    // from all of them — leaving the edge raised for a later frame to fire
    // against whatever is selected by then.
    //
    // Committing on each unheld frame instead, which is what this did, opens and
    // closes a gesture every frame the light is merely selected, and each close
    // is its own undo entry.
    _lightGizmoHeld = held;
    if (held)
    {
        // The signal the end-of-frame sweep reads. Without it the sweep sees no
        // edit in progress, commits the open gesture at the end of every frame of
        // the drag, and the next frame opens a fresh one against the direction
        // this one just wrote — one undo entry per frame, which is what a drag
        // looks like when nothing tells the sweep a drag is happening.
        //
        // The same signal the transform gizmo and the inspector's held widgets
        // raise. Holding the drag below is not a substitute: that governs when
        // *this* code commits, and the sweep commits on its own.
        _captureEditingActive = true;
        _lightDrag.Hold(_selectedEntity, {});
    }
    // That it *drew*, not that it is held. The caller skips the transform gizmo
    // on this, and skipping only while held would let both draw on every frame a
    // sun sat selected — which is the two-Manipulate collision this gating fixes.
    return true;
}

} // namespace Assisi::Editor
