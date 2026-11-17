/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorPostProcessVolumes.cpp
/// @brief Where a box PostProcessVolume applies: its box, and around it, faint,
/// where its blend distance starts it fading in. A volume covering the whole
/// level has no shape to draw.

#include <Assisi/Editor/EditorApp.hpp>

#include <vector>

#include <Assisi/ECS/WorldMatrix.hpp>
#include <Assisi/Editor/Overlay/OverlayRenderer.hpp>
#include <Assisi/Editor/WireShapes.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Runtime/PostProcessVolume.hpp>

namespace Assisi::Editor
{
namespace
{
namespace Rt = Assisi::Runtime;

// A volume has no colour of its own, so every one draws in this, faint enough
// that overlapping volumes stay legible.
constexpr glm::vec4 kVolumeColor{0.75f, 0.55f, 1.0f, 0.4f};
constexpr glm::vec4 kSelectedColor{kSelectionOutline, 1.0f};
constexpr glm::vec4 kActiveSelectedColor{kActiveSelectionOutline, 1.0f};

// The blend distance's outline, as a share of the box's colour's opacity.
constexpr float kBlendOpacity = 0.35f;

// An axis scaled to less than this cannot carry a blend distance out of it.
constexpr float kMinAxisScale = 1.0e-6f;

/// @brief The box, and the box its blend distance reaches out to: each side
/// moved out by the distance in metres, whatever the entity's scale.
void AddVolumeWireframe(std::vector<LineVertex> &out, const glm::mat4 &model, const glm::vec4 &color,
                        const Rt::PostProcessVolume &volume)
{
    const glm::vec3 half = 0.5f * volume.size;
    AddBoxWireframe(out, model, color, half);
    if (volume.blendDistance <= 0.0f)
    {
        return;
    }
    const glm::vec3 scale(glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                          glm::length(glm::vec3(model[2])));
    const glm::vec3 reach = volume.blendDistance / glm::max(scale, glm::vec3(kMinAxisScale));
    AddBoxWireframe(out, model, glm::vec4(glm::vec3(color), color.a * kBlendOpacity), half + reach);
}

} // namespace

void EditorApp::SubmitPostProcessVolumeGizmos()
{
    if (_scene == nullptr)
    {
        return;
    }
    _postProcessVolumeLinesDepthTested.clear();
    _postProcessVolumeLinesOnTop.clear();
    const glm::dvec3 origin = ViewOrigin();

    for (auto [entity, world, volume] : _scene->Query<Assisi::ECS::WorldMatrix, Rt::PostProcessVolume>())
    {
        if (volume.extent != Rt::PostProcessExtent::Box)
        {
            continue;
        }
        const bool selected = IsSelected(entity);
        std::vector<LineVertex> &batch = selected ? _postProcessVolumeLinesOnTop : _postProcessVolumeLinesDepthTested;
        const glm::vec4 color =
            selected ? (entity == _selectedEntity ? kActiveSelectedColor : kSelectedColor) : kVolumeColor;
        AddVolumeWireframe(batch, Assisi::ECS::MatrixFrom(world, origin), color, volume);
    }

    _overlays.SubmitOverlayLines(_postProcessVolumeLinesDepthTested, /*onTop=*/ false);
    _overlays.SubmitOverlayLines(_postProcessVolumeLinesOnTop, /*onTop=*/ true);
}

} // namespace Assisi::Editor
