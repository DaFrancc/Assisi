/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file OverlayRenderer.hpp
/// @brief The marks an editor draws over a scene: selection outlines, entity
///        icons, and wireframe lines.
///
/// None of it means anything to a running game — an outline says "this is what
/// you clicked", an icon says "something is here that has nothing to draw", a
/// wireframe says "this is the collider you cannot otherwise see". They are
/// drawn after the tone map, into a display-encoded target carrying the depth
/// the scene wrote, because their colours are already what they should look like
/// on screen.
///
/// Owned by the editor rather than by SceneRenderer. Existing is what enables
/// them: a game does not construct one, so the passes, their pipelines and the
/// assets under assets/editor/ are absent from its binary rather than skipped by
/// a flag at runtime.

#include <span>
#include <vector>

#include <nvrhi/nvrhi.h>

#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Editor/Overlay/IconPass.hpp>
#include <Assisi/Editor/Overlay/LinePass.hpp>
#include <Assisi/Editor/Overlay/OutlinePass.hpp>
#include <Assisi/Math/GLM.hpp>
#include <Assisi/Render/RenderFrame.hpp>
#include <Assisi/Runtime/Components.hpp>

namespace Assisi::ECS
{
class Scene;
}

namespace Assisi::Runtime
{
class SceneRenderer;
}

namespace Assisi::Editor
{

/// @brief The colour an outlined entity reads in.
inline constexpr glm::vec3 kSelectionOutline{1.0f, 0.55f, 0.1f};

/// @brief The colour the *active* entity reads in — the one the inspector is
/// showing while several are selected together.
///
/// Redder than the rest so the viewport can answer "which one is the inspector
/// talking about" while several entities move together. A single selection is its
/// own active entity, so this is the colour of an ordinary click.
inline constexpr glm::vec3 kActiveSelectionOutline{1.0f, 0.28f, 0.05f};

class OverlayRenderer
{
public:
    /// @brief Build the three overlay pipelines against the display-encoded
    /// overlay target.
    ///
    /// @return false only if nothing came up at all. An individual pass that
    /// fails warns and is dropped: losing entity icons should not cost the
    /// editor its outlines, still less its scene.
    [[nodiscard]] bool Initialize(nvrhi::IDevice *device, const nvrhi::FramebufferInfo &overlayFramebufferInfo,
                                  uint32_t width, uint32_t height);

    /// @brief Rebuild the pipelines for a new overlay target format (an MSAA
    /// toggle, say). Shaders and binding sets are reused.
    [[nodiscard]] bool OnRenderTargetsChanged(const nvrhi::FramebufferInfo &overlayFramebufferInfo);

    /// @brief Mark one entity to receive an always-on-top silhouette outline.
    /// Pass ECS::NullEntity to clear it. The entity must carry a Transform: a
    /// resolved mesh outlines its silhouette, an entity drawn as an icon outlines
    /// that billboard, and anything else is silently skipped.
    void SetHighlightedEntity(ECS::Entity entity)
    {
        _highlightedEntities.clear();
        if (entity != ECS::NullEntity)
        {
            _highlightedEntities.push_back(entity);
        }
        _activeHighlight = entity;
    }

    /// @brief The same, for a multi-entity selection: every one of @p entities
    /// gets its own outline. Replaces whatever was highlighted before; an empty
    /// span clears. Copied rather than referenced — the caller's list is free to
    /// change between here and Render().
    void SetHighlightedEntities(std::span<const ECS::Entity> entities)
    {
        _highlightedEntities.clear();
        for (const ECS::Entity entity : entities)
        {
            if (entity != ECS::NullEntity)
            {
                _highlightedEntities.push_back(entity);
            }
        }
    }

    /// @brief Which highlighted entity is the *active* one — the last clicked,
    /// what the inspector shows and the gizmo drives. It outlines in
    /// kActiveSelectionOutline instead of kSelectionOutline.
    ///
    /// Set separately rather than read off the end of the list above, because the
    /// caller filters that list: an entity already outlined some other way (a
    /// rigidbody, drawn by its collider) is dropped from it, so its last survivor
    /// is not the active entity — it just happens to be last.
    ///
    /// Harmless if it names an entity that is not in the list, or none at all.
    void SetActiveHighlight(ECS::Entity entity) { _activeHighlight = entity; }
    [[nodiscard]] ECS::Entity ActiveHighlight() const { return _activeHighlight; }

    /// @brief The first highlighted entity, or NullEntity. For callers that only
    /// ever set one.
    [[nodiscard]] ECS::Entity HighlightedEntity() const
    {
        return _highlightedEntities.empty() ? ECS::NullEntity : _highlightedEntities.front();
    }

    /// @brief Show/hide the entity icons — world-space billboards marking
    /// entities that have a Transform but nothing to draw. Off by default; an
    /// editor turns them on while authoring and off during play.
    void SetEditorIconsVisible(bool visible) { _editorIconsVisible = visible; }
    [[nodiscard]] bool EditorIconsVisible() const { return _editorIconsVisible; }

    /// @brief Queue a batch of coloured world-space line segments (a LineList —
    /// consecutive vertex pairs). @p onTop routes them to the x-ray pipeline
    /// (visible through geometry); otherwise they are depth-tested. Drawn at the
    /// end of the next Render() and then cleared, so callers re-submit each frame.
    void SubmitOverlayLines(std::span<const LineVertex> vertices, bool onTop);

    /// @brief Suppress the billboard for these entities for the next Render()
    /// (something else already marks them — a collider wireframe, say). Consumed
    /// and cleared each Render(), so the caller re-supplies it every frame.
    void SetIconSuppressedEntities(std::span<const ECS::Entity> entities);

    /// @brief Queue billboards at world positions that belong to no entity.
    ///
    /// A blueprint instance's root is exactly that: it evaporates at expansion, so
    /// no Transform anywhere in the scene marks where the copy was placed — and
    /// without a mark the author has nothing to click and nothing to look at while
    /// dragging the group. The positions come from the world's instance table,
    /// which is editor knowledge, so they are pushed in rather than queried out.
    ///
    /// Consumed and cleared each Render(); re-submit every frame. Skipped entirely
    /// when icons are hidden.
    void SubmitEditorIcons(std::span<const glm::vec3> positions);

    /// @brief Outline one of those billboards, so a selected instance reads the
    /// same as a selected entity. Cleared each Render(); re-submit every frame.
    void SubmitIconOutline(const glm::vec3 &position);

    /// @brief Queue one independent silhouette outline for the next Render(): the
    /// @p items' silhouettes union into a SINGLE border of @p color (a capsule
    /// collider is a cylinder plus two spheres, which must merge). Each call is
    /// its own edge-detect pass, so separate groups never combine — a collider and
    /// the mesh it wraps outline independently. Cleared each Render().
    void SubmitOutlineGroup(std::span<const OutlinePass::OutlineItem> items, const glm::vec3 &color);

    /// @brief Convenience for a single-mesh outline group. See SubmitOutlineGroup.
    void SubmitOutline(const Render::MeshBuffer *mesh, const glm::mat4 &model, const glm::vec3 &color);

    /// @brief Draw everything queued, for the scene @p sceneRenderer just drew.
    ///
    /// @p sceneRenderer is consulted for the LOD each mesh was actually drawn at:
    /// a border traced around a finer silhouette than the one on screen reads as a
    /// halo, so the outline asks rather than deciding for itself.
    void Render(const Render::RenderFrame &frame, ECS::Scene &scene, const Runtime::Transform &cameraTransform,
                const Runtime::Camera &camera, const Runtime::SceneRenderer &sceneRenderer);

private:
    void DrawEditorIcons(const Render::RenderFrame &frame, const glm::mat4 &viewProjection, const glm::mat4 &view,
                         const glm::vec3 &cameraPosition, ECS::Scene &scene);

    void DrawHighlightOutline(const Render::RenderFrame &frame, const glm::mat4 &viewProjection, const glm::mat4 &view,
                              ECS::Scene &scene, const Runtime::SceneRenderer &sceneRenderer);

    void DrawHighlightOutlineFor(const Render::RenderFrame &frame, const glm::mat4 &viewProjection,
                                 const glm::mat4 &view, ECS::Entity entity, const Runtime::SceneRenderer &sceneRenderer,
                                 ECS::Scene &scene);

    [[nodiscard]] bool IsIconSuppressed(ECS::Entity entity) const;

    OutlinePass _outlinePass;
    IconPass _iconPass;
    LinePass _linePass;

    /// The entities drawn with a selection outline this frame (empty = none).
    std::vector<ECS::Entity> _highlightedEntities;

    /// Reused scratch for the per-frame icon positions, so drawing icons does not
    /// allocate every frame.
    std::vector<glm::vec3> _iconPositions;

    /// Overlay line segments queued this frame, split by depth mode; drawn at the
    /// end of Render() and cleared. Refilled each frame by the caller.
    std::vector<LineVertex> _overlayLinesDepthTested;
    std::vector<LineVertex> _overlayLinesOnTop;

    /// Independent silhouette-outline groups queued this frame; each is drawn as
    /// its own edge-detect pass (so a collider and its mesh never merge), then
    /// cleared.
    struct OutlineGroup
    {
        std::vector<OutlinePass::OutlineItem> items;
        glm::vec3 color;
    };
    std::vector<OutlineGroup> _outlineGroups;

    /// Entities whose icon is suppressed this frame (drawn some other way).
    /// Consumed and cleared each Render().
    std::vector<ECS::Entity> _iconSuppressed;

    /// Billboards belonging to no entity — a blueprint instance's root, which has
    /// no Transform in the scene to be found by a query. Pushed in by the caller
    /// and cleared each Render(). See SubmitEditorIcons.
    std::vector<glm::vec3> _submittedIcons;
    std::vector<glm::vec3> _submittedIconOutlines;

    /// Which highlighted entity outlines in the active colour. See
    /// SetActiveHighlight.
    ECS::Entity _activeHighlight = ECS::NullEntity;

    bool _editorIconsVisible = false;
};

} // namespace Assisi::Editor
