/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file EditorOptions.cpp
/// @brief EditorOptionsPanel::Draw — the body of the F11 options overlay.
///
/// Everything the panel is and why it lives here is documented on the class, in
/// EditorOptionsPanel.hpp. This file is the layout, top to bottom: frame-time
/// readouts, GPU telemetry, the frame graph, percentile stats, the renderer A/B
/// toggles, then the persisted anti-aliasing and frame-sync settings.

#include "EditorOptionsPanel.hpp"

#include <Assisi/App/Application.hpp>
#include <Assisi/Runtime/SceneRenderer.hpp>

#include <Assisi/App/OptionsConfig.hpp>
#include <Assisi/Render/PostProcess.hpp>
#include <Assisi/Window/InputContext.hpp>
#include <Assisi/Window/Key.hpp>

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Assisi::Editor
{

using Assisi::App::Application;
using Assisi::App::FrameSyncMode;
using Assisi::App::OptionsConfig;

namespace
{
/// Who holds which rectangle of the local-light atlas, and how long each one's
/// still layer has stood.
///
/// The safety argument for caching, not polish. A missed invalidation has no
/// visual tell — a shadow simply stops following its object — so the one thing
/// that makes it findable is being able to see that a tile has not been redrawn
/// while the thing under it moved. An age that keeps climbing on a light
/// something is walking under is the defect, on screen, as a number.
void DrawShadowAtlasInspector(std::span<const Assisi::Render::LocalShadowCache::Residency> tiles)
{
    if (!ImGui::TreeNode("Atlas Tiles"))
    {
        return;
    }
    if (tiles.empty())
    {
        ImGui::TextUnformatted("No light holds a tile.");
        ImGui::TreePop();
        return;
    }

    if (ImGui::BeginTable("atlas-tiles", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Light");
        ImGui::TableSetupColumn("Tile");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Age");
        ImGui::TableHeadersRow();

        for (const Assisi::Render::LocalShadowCache::Residency &tile : tiles)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s %u", tile.kind == Assisi::Render::LocalLightKind::Point ? "point" : "spot",
                        tile.lightIndex);
            ImGui::TableNextColumn();
            // The first face's corner. A point light's other five are the
            // allocator's business and naming one of six locates the light.
            ImGui::Text("%u,%u%s", tile.rect[0].x, tile.rect[0].y, tile.faces > 1 ? " +5" : "");
            ImGui::TableNextColumn();
            ImGui::Text("%u", Assisi::Render::ShadowSizeClassResolution(tile.sizeClass));
            ImGui::TableNextColumn();
            ImGui::Text("%u", tile.ageFrames);
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}

/// Every shadow-casting local light and what became of it.
///
/// The rows nobody wants are the reason it exists: a light that lost its shadow
/// is invisible in the picture except as "that lamp looks wrong", and which
/// lights lose theirs changes as the camera moves. Each state names a different
/// setting, so the column is the fix as much as the diagnosis.
void DrawShadowLightReport(const Assisi::Render::ShadowDiagnostics &diagnostics)
{
    if (!ImGui::TreeNode("Shadowed Lights"))
    {
        return;
    }
    if (diagnostics.lights.empty())
    {
        ImGui::TextUnformatted("No local light casts a shadow.");
        ImGui::TreePop();
        return;
    }

    if (ImGui::BeginTable("shadow-lights", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Light");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Tile");
        ImGui::TableSetupColumn("Score");
        ImGui::TableHeadersRow();

        for (const Assisi::Render::LocalShadowLightReport &light : diagnostics.lights)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s %u", light.kind == Assisi::Render::LocalLightKind::Point ? "point" : "spot",
                        light.lightIndex);

            ImGui::TableNextColumn();
            // Coloured only where something is off the fast path. Colouring the
            // ordinary case too would make a healthy scene a wall of green and
            // leave the one row that matters no easier to find. Shadows switched
            // off is not off the fast path either — it is an answered question,
            // and the checkbox that answered it is a few lines above.
            const bool verdict = light.state != Assisi::Render::LocalShadowState::Shadowed &&
                                 light.state != Assisi::Render::LocalShadowState::Disabled;
            if (verdict)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, light.state == Assisi::Render::LocalShadowState::Demoted
                                                         ? ImVec4{1.f, 0.82f, 0.4f, 1.f}
                                                         : ImVec4{1.f, 0.5f, 0.4f, 1.f});
            }
            ImGui::TextUnformatted(Assisi::Render::LocalShadowStateName(light.state));
            if (verdict)
            {
                ImGui::PopStyleColor();
            }
            ImGui::SetItemTooltip("%s", Assisi::Render::DescribeLocalShadowState(light.state));

            ImGui::TableNextColumn();
            if (light.resolution == 0u)
            {
                ImGui::TextUnformatted("—");
            }
            else if (light.state == Assisi::Render::LocalShadowState::Demoted)
            {
                ImGui::Text("%u (asked %u)", light.resolution, light.requestedResolution);
            }
            else
            {
                ImGui::Text("%u", light.resolution);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.3f", static_cast<double>(light.score));
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}
} // namespace

// The sun's cascades. Everything here lands on the next frame: a resolution or
// cascade-count change reallocates the array in SceneRenderer::Render, and the
// rest ride into the shader as frame constants — so a knob can be compared
// against its neighbour without anything being rebuilt between the two.
void EditorOptionsPanel::DrawShadowSettings(const Frame &frame)
{
    Assisi::Render::ShadowSettings shadows = frame.renderer.ShadowSettings();
    bool changed = false;

    ImGui::TextUnformatted("Sun Shadows");

    changed |= ImGui::Checkbox("Cast Shadows", &shadows.sun.enabled);

    // The tier presets from the quality table. A tier is a preset over the knobs
    // below, never a lock on them: pressing one writes those knobs and they stay
    // editable, which is why the readout falls to Custom after any edit rather
    // than remembering which button was last pressed.
    static const char *kTierNames[] = {"Low", "Medium", "High", "Ultra"};
    const Assisi::Render::ShadowTier tier = Assisi::Render::Tier(shadows);
    for (int32_t i = 0; i < IM_ARRAYSIZE(kTierNames); ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine();
        }
        const bool active = static_cast<int32_t>(tier) == i;
        ImGui::BeginDisabled(active);
        if (ImGui::SmallButton(kTierNames[i]))
        {
            // The tier's own knobs only — the biases and the blend band are
            // correctness settings no tier has an opinion about, so an edit to
            // one survives a tier change.
            const Assisi::Render::ShadowSettings preset =
                Assisi::Render::TierSettings(static_cast<Assisi::Render::ShadowTier>(i));
            shadows.sun.cascadeCount = preset.sun.cascadeCount;
            shadows.sun.resolution = preset.sun.resolution;
            shadows.sun.format = preset.sun.format;
            shadows.sun.maxDistance = preset.sun.maxDistance;
            shadows.sun.filter = preset.sun.filter;
            // The local half too: a tier is one point in the whole knob space,
            // and writing half of it would leave the readout reporting Custom
            // the moment it was pressed.
            shadows.local.atlasResolution = preset.local.atlasResolution;
            shadows.local.format = preset.local.format;
            shadows.local.faceResolution = preset.local.faceResolution;
            shadows.local.filter = preset.local.filter;
            shadows.selection.capSpot = preset.selection.capSpot;
            shadows.selection.capPoint = preset.selection.capPoint;
            shadows.pcss = preset.pcss;
            changed = true;
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    // The sun's own bytes, not the whole knob space's: this section is the sun,
    // and quoting an atlas nothing has allocated would be a figure with no
    // memory behind it.
    ImGui::TextDisabled("%s  (%.0f MiB)", tier == Assisi::Render::ShadowTier::Custom ? "Custom" : "",
                        static_cast<double>(Assisi::Render::SunShadowMemoryBytes(shadows.sun)) / (1024.0 * 1024.0));

    // Above both halves rather than inside either, because one setting names
    // both: it stays editable with the sun off, for the lamps.
    static const char *const kPcssNames[] = {"Off", "Sun", "Sun + Local Lights"};
    static_assert(std::size(kPcssNames) == static_cast<std::size_t>(Assisi::Render::ShadowPcss::Count),
                  "The contact hardening list must name every ShadowPcss.");
    int32_t pcssIndex = static_cast<int32_t>(shadows.pcss);
    if (ImGui::Combo("Contact Hardening", &pcssIndex, kPcssNames, static_cast<int32_t>(std::size(kPcssNames))))
    {
        shadows.pcss = static_cast<Assisi::Render::ShadowPcss>(pcssIndex);
        changed = true;
    }
    ImGui::SetItemTooltip("Sizes each shadow's softness from how far its caster is above it: sharp where things "
                          "touch, soft where a shadow falls far from what casts it. Replaces the filter below "
                          "with a 16-tap disk and adds a 16-fetch search.");

    if (!shadows.sun.enabled)
    {
        ImGui::BeginDisabled();
    }

    int32_t cascadeCount = static_cast<int32_t>(shadows.sun.cascadeCount);
    if (ImGui::SliderInt("Cascades", &cascadeCount, static_cast<int32_t>(Assisi::Render::kMinShadowCascades),
                         static_cast<int32_t>(Assisi::Render::kMaxShadowCascades)))
    {
        shadows.sun.cascadeCount = static_cast<std::uint32_t>(cascadeCount);
        changed = true;
    }

    // Powers of two, because the texel lattice the cascade snaps to has to
    // divide the box evenly (see Render::Sanitized).
    static const char *kResolutionNames[] = {"512", "1024", "2048", "4096"};
    static const std::uint32_t kResolutions[] = {512u, 1024u, 2048u, 4096u};
    int32_t resolutionIndex = 2;
    for (int32_t i = 0; i < IM_ARRAYSIZE(kResolutions); ++i)
    {
        if (kResolutions[i] == shadows.sun.resolution)
        {
            resolutionIndex = i;
        }
    }
    if (ImGui::Combo("Resolution", &resolutionIndex, kResolutionNames, IM_ARRAYSIZE(kResolutionNames)))
    {
        shadows.sun.resolution = kResolutions[resolutionIndex];
        changed = true;
    }

    // **Indexed by the enum value** — this list must stay in
    // Render::ShadowMapFormat's order.
    static const char *kFormatNames[] = {"D16", "D32"};
    int32_t formatIndex = static_cast<int32_t>(shadows.sun.format);
    if (ImGui::Combo("Depth Format", &formatIndex, kFormatNames, IM_ARRAYSIZE(kFormatNames)))
    {
        shadows.sun.format = static_cast<Assisi::Render::ShadowMapFormat>(formatIndex);
        changed = true;
    }

    // **Indexed by the enum value** — this list must stay in
    // Render::ShadowFilter's order.
    static const char *kFilterNames[] = {"1 tap", "3x3 PCF", "5x5 PCF", "Vogel (16 tap)"};
    int32_t filterIndex = static_cast<int32_t>(shadows.sun.filter);
    if (ImGui::Combo("Filter", &filterIndex, kFilterNames, IM_ARRAYSIZE(kFilterNames)))
    {
        shadows.sun.filter = static_cast<Assisi::Render::ShadowFilter>(filterIndex);
        changed = true;
    }

    changed |= ImGui::SliderFloat("Distance", &shadows.sun.maxDistance, Assisi::Render::kMinShadowDistance,
                                  Assisi::Render::kMaxShadowDistance, "%.0f m");
    // 1 is fully logarithmic (near cascades get most of the resolution), 0 fully
    // uniform (they get almost none).
    changed |= ImGui::SliderFloat("Split Lambda", &shadows.sun.splitLambda, Assisi::Render::kMinSplitLambda,
                                  Assisi::Render::kMaxSplitLambda, "%.2f");
    changed |= ImGui::SliderFloat("Cascade Blend", &shadows.sun.cascadeBlend, Assisi::Render::kMinCascadeBlend,
                                  Assisi::Render::kMaxCascadeBlend, "%.2f");

    // Both biases are quoted in texels and scaled per cascade by that cascade's
    // world-per-texel, so one number holds across all of them. Raise the depth
    // bias until acne clears; if the shadow detaches at contact before it does,
    // that is the normal offset's job instead.
    changed |= ImGui::SliderFloat("Depth Bias", &shadows.sun.depthBiasTexels, Assisi::Render::kMinDepthBiasTexels,
                                  Assisi::Render::kMaxDepthBiasTexels, "%.2f texels");
    changed |=
        ImGui::SliderFloat("Normal Offset", &shadows.sun.normalOffsetTexels, Assisi::Render::kMinNormalOffsetTexels,
                           Assisi::Render::kMaxNormalOffsetTexels, "%.2f texels");
    changed |= ImGui::SliderFloat("Slope Bias", &shadows.sun.slopeBias, Assisi::Render::kMinSlopeBias,
                                  Assisi::Render::kMaxSlopeBias, "%.2f");

    changed |= ImGui::Checkbox("Keep Still Cascades", &shadows.sun.cadence.enabled);
    ImGui::SetItemTooltip("Keeps a cascade's depth instead of drawing it again while its fit has not moved and "
                          "nothing has moved inside it, which skips the caster walk as well as the draw. Off is "
                          "the per-frame baseline, exactly: every cascade, every frame.");

    if (!shadows.sun.cadence.enabled)
    {
        ImGui::BeginDisabled();
    }
    changed |= ImGui::SliderFloat("Cascade Drift", &shadows.sun.cadence.driftTexels,
                                  Assisi::Render::kMinCascadeDriftTexels, Assisi::Render::kMaxCascadeDriftTexels,
                                  "%.2f texels");
    ImGui::SetItemTooltip("How far a shadow edge may slide before its cascade is drawn again, in that cascade's "
                          "own texels. Zero keeps a cascade only while its fit is unchanged; half a texel is "
                          "inside the softness the cheapest filter already has.");
    if (!shadows.sun.cadence.enabled)
    {
        ImGui::EndDisabled();
    }

    if (!shadows.sun.enabled)
    {
        ImGui::EndDisabled();
    }

    // Diagnostics, not settings: runtime only and never persisted. Each one
    // changes the picture, which is why they are a list with an off entry rather
    // than checkboxes that could be left on by accident.
    static const char *const kShadowDebugNames[] = {"Off", "Cascades", "Occluder Margin", "Filter Taps"};
    static_assert(std::size(kShadowDebugNames) == Assisi::Render::kShadowDebugViewCount,
                  "The debug view list must name every ShadowDebugView.");
    int debugView = static_cast<int>(frame.renderer.ShadowDebugView());
    if (ImGui::Combo("Shadow View", &debugView, kShadowDebugNames,
                     static_cast<int>(Assisi::Render::kShadowDebugViewCount)))
    {
        frame.renderer.SetShadowDebugView(static_cast<Assisi::Render::ShadowDebugView>(debugView));
    }

    const Assisi::Render::ShadowPass::Stats stats = frame.renderer.LastShadowStats();
    ImGui::Text("Cascades: %u  |  %u instances / %u batches", stats.cascades, stats.instances, stats.batches);
    ImGui::Text("Casters culled: %u  |  %u draw calls", stats.culled, stats.drawCalls);
    // Reads zero on a frame whose cascades were all kept, so it is worth
    // reading with the camera moving.
    ImGui::Text("Shadow LOD: %u caster-cascade pairs one level coarser", frame.renderer.LastShadowLodCoarser());

    ImGui::Separator();
    ImGui::TextUnformatted("Local Light Shadows");

    changed |= ImGui::Checkbox("Cast Shadows##local", &shadows.local.enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(%.0f MiB)",
                        static_cast<double>(Assisi::Render::LocalShadowMemoryBytes(shadows.local)) / (1024.0 * 1024.0));

    if (!shadows.local.enabled)
    {
        ImGui::BeginDisabled();
    }

    static const char *const kAtlasNames[] = {"512", "1024", "2048", "4096", "8192"};
    static constexpr std::uint32_t kAtlasResolutions[] = {512u, 1024u, 2048u, 4096u, 8192u};
    static_assert(std::size(kAtlasNames) == std::size(kAtlasResolutions), "Every atlas resolution needs a label.");
    int32_t atlasIndex = 0;
    for (std::size_t i = 0; i < std::size(kAtlasResolutions); ++i)
    {
        if (kAtlasResolutions[i] == shadows.local.atlasResolution)
        {
            atlasIndex = static_cast<int32_t>(i);
        }
    }
    if (ImGui::Combo("Atlas", &atlasIndex, kAtlasNames, static_cast<int32_t>(std::size(kAtlasNames))))
    {
        shadows.local.atlasResolution = kAtlasResolutions[atlasIndex];
        changed = true;
    }

    int32_t localFormatIndex = static_cast<int32_t>(shadows.local.format);
    if (ImGui::Combo("Depth Format##local", &localFormatIndex, kFormatNames, IM_ARRAYSIZE(kFormatNames)))
    {
        shadows.local.format = static_cast<Assisi::Render::ShadowMapFormat>(localFormatIndex);
        changed = true;
    }
    ImGui::SetItemTooltip("D32 doubles the atlas's memory. A local light's depth range is its own reach, so D16 "
                          "is millimetres there.");

    static const char *const kFaceNames[] = {"128", "256", "512", "1024", "2048"};
    static constexpr std::uint32_t kFaceResolutions[] = {128u, 256u, 512u, 1024u, 2048u};
    static_assert(std::size(kFaceNames) == std::size(kFaceResolutions), "Every face class needs a label.");
    int32_t faceIndex = 0;
    for (std::size_t i = 0; i < std::size(kFaceResolutions); ++i)
    {
        if (kFaceResolutions[i] == shadows.local.faceResolution)
        {
            faceIndex = static_cast<int32_t>(i);
        }
    }
    if (ImGui::Combo("Face Size", &faceIndex, kFaceNames, static_cast<int32_t>(std::size(kFaceNames))))
    {
        shadows.local.faceResolution = kFaceResolutions[faceIndex];
        changed = true;
    }
    ImGui::SetItemTooltip("The tile a light's face gets before anything demotes it. A light too far away to "
                          "fill one takes a smaller tile on its own.");

    int32_t localFilterIndex = static_cast<int32_t>(shadows.local.filter);
    if (ImGui::Combo("Filter##local", &localFilterIndex, kFilterNames,
                     static_cast<int32_t>(Assisi::Render::kShadowFilterCount)))
    {
        shadows.local.filter = static_cast<Assisi::Render::ShadowFilter>(localFilterIndex);
        changed = true;
    }

    changed |= ImGui::SliderFloat("Source Radius##local", &shadows.local.sourceRadius,
                                  Assisi::Render::kMinLocalSourceRadius, Assisi::Render::kMaxLocalSourceRadius,
                                  "%.2f m");
    ImGui::SetItemTooltip("How large every spot and point light's emitter is taken to be when contact hardening "
                          "sizes its shadows. Zero is a point source, hard everywhere. Unread unless contact "
                          "hardening covers local lights.");

    changed |=
        ImGui::SliderFloat("Depth Bias##local", &shadows.local.depthBiasTexels, Assisi::Render::kMinDepthBiasTexels,
                           Assisi::Render::kMaxDepthBiasTexels, "%.2f texels");
    changed |= ImGui::SliderFloat("Normal Offset##local", &shadows.local.normalOffsetTexels,
                                  Assisi::Render::kMinNormalOffsetTexels, Assisi::Render::kMaxNormalOffsetTexels,
                                  "%.2f texels");
    changed |= ImGui::SliderFloat("Slope Bias##local", &shadows.local.slopeBias, Assisi::Render::kMinSlopeBias,
                                  Assisi::Render::kMaxSlopeBias, "%.2f");

    changed |= ImGui::Checkbox("Cache Static Depth", &shadows.local.cache.enabled);
    ImGui::SetItemTooltip("Keeps the still geometry's depth in each tile instead of redrawing it, so a lamp with "
                          "nothing moving under it costs nothing per frame. Costs a second atlas. Off is the "
                          "uncached baseline, exactly: every face of every served light, every frame.");

    if (!shadows.local.cache.enabled)
    {
        ImGui::BeginDisabled();
    }
    int32_t budget = static_cast<int32_t>(shadows.local.cache.updateBudgetFaces);
    if (ImGui::SliderInt("Redraw Budget", &budget, static_cast<int32_t>(Assisi::Render::kMinShadowBakeBudget),
                         static_cast<int32_t>(Assisi::Render::kMaxShadowBakeBudget), "%d faces"))
    {
        shadows.local.cache.updateBudgetFaces = static_cast<std::uint32_t>(budget);
        changed = true;
    }
    ImGui::SetItemTooltip("Faces that may be redrawn in one frame, most important first — what stops walking "
                          "into a new room landing as one long frame. A light that does not fit waits, "
                          "unshadowed, rather than showing a tile that is out of date.");

    int32_t stillFrames = static_cast<int32_t>(shadows.local.cache.promoteStillFrames);
    if (ImGui::SliderInt("Settle Frames", &stillFrames, static_cast<int32_t>(Assisi::Render::kMinPromoteStillFrames),
                         static_cast<int32_t>(Assisi::Render::kMaxPromoteStillFrames)))
    {
        shadows.local.cache.promoteStillFrames = static_cast<std::uint32_t>(stillFrames);
        changed = true;
    }
    ImGui::SetItemTooltip("How long a caster must hold still before it is folded back into the kept layer. A "
                          "motion costs two redraws however long it lasts — one leaving, one rejoining — and "
                          "this is the wait before the second.");

    int32_t divisor = static_cast<int32_t>(shadows.local.cache.movingLightUpdateDivisor);
    if (ImGui::SliderInt("Mover Update Rate", &divisor, static_cast<int32_t>(Assisi::Render::kMinLightUpdateDivisor),
                         static_cast<int32_t>(Assisi::Render::kMaxLightUpdateDivisor), "every %d frame(s)"))
    {
        shadows.local.cache.movingLightUpdateDivisor = static_cast<std::uint32_t>(divisor);
        changed = true;
    }
    ImGui::SetItemTooltip("A ceiling, not a rate: the most important lights always redraw every frame, and "
                          "this is spent further down the ordering. One throttles nothing.");
    if (!shadows.local.cache.enabled)
    {
        ImGui::EndDisabled();
    }

    changed |= ImGui::Checkbox("Importance Cap", &shadows.selection.capEnabled);
    ImGui::SetItemTooltip("Off does not lift the limit, only the ordering: the atlas is a fixed-size texture "
                          "either way, so lights are served until it is full and the rest go unshadowed in "
                          "the order they were placed rather than by what they contribute. Worse under "
                          "pressure, and your call.");

    if (!shadows.selection.capEnabled)
    {
        ImGui::BeginDisabled();
    }
    int32_t capSpot = static_cast<int32_t>(shadows.selection.capSpot);
    if (ImGui::SliderInt("Shadowed Spots", &capSpot, static_cast<int32_t>(Assisi::Render::kMinShadowCap),
                         static_cast<int32_t>(Assisi::Render::kMaxShadowCap)))
    {
        shadows.selection.capSpot = static_cast<std::uint32_t>(capSpot);
        changed = true;
    }
    int32_t capPoint = static_cast<int32_t>(shadows.selection.capPoint);
    if (ImGui::SliderInt("Shadowed Points", &capPoint, static_cast<int32_t>(Assisi::Render::kMinShadowCap),
                         static_cast<int32_t>(Assisi::Render::kMaxShadowCap)))
    {
        shadows.selection.capPoint = static_cast<std::uint32_t>(capPoint);
        changed = true;
    }
    ImGui::SetItemTooltip("Fewer than the spot cap, and deliberately: a point light is six shadow renders "
                          "against a spot's one.");
    if (!shadows.selection.capEnabled)
    {
        ImGui::EndDisabled();
    }

    if (!shadows.local.enabled)
    {
        ImGui::EndDisabled();
    }

    const Assisi::Render::LocalShadowPass::Stats local = frame.renderer.LastLocalShadowStats();
    const Assisi::Render::ShadowDiagnostics &diagnostics = frame.renderer.ShadowDiagnostics();

    // The headline: how many of the lights that asked for a shadow are getting
    // one. Any gap here is what the rest of this section explains.
    ImGui::Text("Shadowed: %u of %u lights", diagnostics.shadowedLights, diagnostics.castingLights);
    ImGui::SetItemTooltip("Local lights whose castsShadows is on, against the ones actually holding an atlas "
                          "tile this frame. A gap is not a bug — it is one of the mechanisms below bounding "
                          "cost — but it is never meant to be a surprise.");

    ImGui::Text("Atlas: %u lights / %u faces  |  %.0f%% full", local.lights, local.views,
                static_cast<double>(local.occupancy) * 100.0);
    // Three different answers to "why has that lamp no shadow", and they are
    // three different settings: the cap turned it away, the atlas had no room,
    // or the redraw budget has not reached it yet.
    ImGui::Text("Dropped by cap: %u  |  atlas full: %u  |  waiting to redraw: %u",
                frame.renderer.LastShadowDroppedByCap(), local.unserved, local.deferredLights);

    // The sun's share, split by cascade. The total says the sun costs more than
    // it did; the split says whether that came from the near detail or the far
    // distance, which are different things to fix.
    if (diagnostics.cascadeCount > 0)
    {
        const Assisi::Render::ShadowPass::Stats sun = frame.renderer.LastShadowStats();
        std::string cascades;
        std::string ages;
        for (std::uint32_t cascade = 0; cascade < diagnostics.cascadeCount; ++cascade)
        {
            const char *const separator = cascade == 0 ? "" : " / ";
            cascades += separator;
            cascades += std::to_string(sun.cascadeCasters[cascade]);
            ages += separator;
            ages += std::to_string(diagnostics.cascadeAgeFrames[cascade]);
        }
        ImGui::Text("Cascade casters: %s", cascades.c_str());
        ImGui::SetItemTooltip("Casters drawn into each sun cascade, nearest first. A caster reaching several "
                              "cascades is counted in each, because it is drawn into each. A cascade that kept "
                              "its depth this frame drew nothing and reads zero.");

        // The sun's half of the pay-for-what-you-place reading. Standing still
        // under a fixed sun this settles at "0 of 4 cascades drawn" with the
        // ages climbing; a cascade whose age never climbs while nothing moves in
        // it is the cadence invalidating something that did not change.
        ImGui::Text("Cascades drawn: %u of %u  |  ages %s", diagnostics.cascadesRedrawn, diagnostics.cascadeCount,
                    ages.c_str());
        ImGui::SetItemTooltip("Frames since each cascade was last drawn, nearest first. A near cascade trips on "
                              "a step of the camera and a far one hardly at all, which is where the saving is — "
                              "and the far cascades are the expensive ones.");

        // Why the cascades are or are not settling, beside the reading itself.
        // "Never settles" is EXPECTED at a fast day rate — at six seconds a day
        // the sun turns a degree a frame, which is four orders past the drift
        // tolerance — and without this line that reads as the cadence being
        // broken. Zero cascades for a long stretch is polar night, which is also
        // expected and also looks wrong.
        const Assisi::Runtime::SkyResolution &sky = frame.renderer.LastSky();
        const char *body = sky.light.body == Assisi::Runtime::LightingBody::Sun     ? "Sun"
                           : sky.light.body == Assisi::Runtime::LightingBody::Moon  ? "Moon"
                                                                                    : "nothing";
        ImGui::Text("Lit by: %s  |  sun %.4f deg/frame at 60 Hz", body,
                    static_cast<double>(glm::degrees(sky.sunAngularVelocity)) / 60.0);
        ImGui::SetItemTooltip("How far the sun turns per frame at the current day length. Past about a "
                              "hundredth of a degree every cascade is redrawn every frame, which is the "
                              "cadence working rather than failing. Nothing lighting means polar night or a "
                              "moonless night, and no cascades are drawn at all.");
    }

    if (shadows.local.cache.enabled)
    {
        // The number the pay-for-what-you-place gate is read off, per light: on
        // a still scene every served light is resting and both draws are zero.
        //
        // Every count carries its unit, and they are three different ones. A
        // point light is six faces, so resting lights and copied faces are an
        // order of magnitude apart while describing the same lights — printed
        // bare, side by side, they read as a contradiction that is not there.
        ImGui::Text("Cache: %u of %u lights resting  |  %u baked / %u copied faces  |  %u moving casters",
                    local.restingLights, local.lights, local.bakedFaces, local.copiedFaces, local.dynamicCasters);
        ImGui::SetItemTooltip("Resting is per light and means nothing is moving within its reach. Baked and "
                              "copied are per face — six of them for every point light — so the two counts are "
                              "not comparable and are not meant to be.");

        // The burst condition, said only when it happens. Walking into a room
        // saturating the budget for a frame is the mechanism working; the same
        // line standing still is content that has outrun it, and that is the
        // thing that would otherwise show up as an unexplained hitch.
        if (diagnostics.BudgetSaturated())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.f, 0.82f, 0.4f, 1.f});
            ImGui::Text("Redraw budget saturated: %u of %u faces deferred", diagnostics.deferredFaces,
                        diagnostics.budgetFaces);
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Expected for a frame or two when a room opens onto many lights. If it "
                                  "persists while nothing is moving, raise the redraw budget — the deferred "
                                  "lights are unshadowed until it reaches them.");
        }

        DrawShadowAtlasInspector(frame.renderer.CachedShadowTiles());
    }

    DrawShadowLightReport(diagnostics);

    if (changed)
    {
        frame.renderer.SetShadowSettings(shadows);
        frame.options.shadows = shadows;
        frame.options.SaveToJson();
    }
}

// The sky probe. Off is the A/B against the whole feature and draws exactly
// what a sky drew before there was one; the rest land on the next frame, a
// resolution change by reallocating the probe and anything that changes a bake
// by baking again.
void EditorOptionsPanel::DrawEnvironmentSettings(const Frame &frame)
{
    Assisi::Render::EnvironmentSettings environment = frame.renderer.EnvironmentSettings();
    bool changed = false;

    ImGui::TextUnformatted("Sky Reflections");
    changed |= ImGui::Checkbox("Reflect the Sky", &environment.enabled);

    if (!environment.enabled)
    {
        ImGui::BeginDisabled();
    }

    // Face sizes the combo offers, each a power of two inside the sanitized
    // range. The end points are left to options.json: below 64 the horizon
    // blurs in a mirror, and above 256 a disk-less sky has nothing left to show.
    static constexpr uint32_t kResolutions[] = {64u, 128u, 256u};
    static const char *kResolutionNames[] = {"64", "128", "256"};
    int32_t resolutionIndex = 0;
    for (int32_t i = 0; i < static_cast<int32_t>(IM_ARRAYSIZE(kResolutions)); ++i)
    {
        if (kResolutions[i] == environment.resolution)
        {
            resolutionIndex = i;
        }
    }
    if (ImGui::Combo("Probe Size", &resolutionIndex, kResolutionNames, IM_ARRAYSIZE(kResolutionNames)))
    {
        environment.resolution = kResolutions[resolutionIndex];
        changed = true;
    }

    int32_t samples = static_cast<int32_t>(environment.sampleCount);
    if (ImGui::SliderInt("Probe Samples", &samples, static_cast<int32_t>(Assisi::Render::kMinProbeSampleCount),
                         static_cast<int32_t>(Assisi::Render::kMaxProbeSampleCount)))
    {
        environment.sampleCount = static_cast<uint32_t>(samples);
        changed = true;
    }
    ImGui::SetItemTooltip("Per texel at the first blurred mip; each rougher mip takes four times as many.");

    changed |= ImGui::SliderFloat("Rebake Angle", &environment.rebakeDegrees, 0.f,
                                  Assisi::Render::kMaxProbeRebakeDegrees, "%.2f deg");
    ImGui::SetItemTooltip("How far the sun or moon may move before the sky is captured again. "
                          "Zero bakes on every change.");

    if (!environment.enabled)
    {
        ImGui::EndDisabled();
    }

    const Assisi::Render::SkyProbe &probe = frame.renderer.SkyProbe();
    if (probe.Resolution() == 0u)
    {
        // Said rather than left blank: a scene with no sky, or a pinned
        // ambient, holds no probe whatever the switch says.
        ImGui::TextDisabled("No probe: nothing in the scene is lit by a sky.");
    }
    else
    {
        const Assisi::Render::SkyProbe::Stats &stats = probe.LastStats();
        ImGui::Text("%ux%u, %u mips, %u bakes, kept %u frames", probe.Resolution(), probe.Resolution(),
                    probe.MipCount(), stats.bakes, stats.ageFrames);
    }

    if (changed)
    {
        frame.renderer.SetEnvironmentSettings(environment);
        frame.options.environment = frame.renderer.EnvironmentSettings();
        frame.options.SaveToJson();
    }
}

bool EditorOptionsPanel::Draw(const Frame &frame)
{
    bool applyDisplay = false;

    // The toggle lives here rather than in the engine, so nothing reserves F11 and a
    // game can rebind or drop it.
    if (frame.input.IsKeyPressed(Assisi::Window::Key::F11))
    {
        _showOptions = !_showOptions;
    }

    if (!_showOptions)
    {
        return false;
    }

    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Options", &_showOptions))
    {
        const int32_t frameHistory = static_cast<int32_t>(frame.cpuMs.size());

        // CPU against GPU frame time: whichever dominates is what the frame is bound
        // by. Both are averaged over the same window as the FPS counter; the plots
        // further down are raw per-frame samples, so spikes survive.
        const int32_t fps = frame.fps;
        ImGui::Text("CPU: %5.2f ms    GPU: %5.2f ms", frame.cpuFrameMs, frame.gpuFrameMs);
        ImGui::Text("Frame: %5.2f ms (%d FPS)", fps > 0 ? 1000.0 / fps : 0.0, fps);

        // Hardware telemetry (NVIDIA/NVML), next to the GPU frame time because it
        // explains it: a capped frame rate downclocks the GPU, so identical work
        // takes longer and GPU-ms rises with nothing about the scene changed. Low
        // clock and power beside a high GPU-ms is that, not a regression.
        const Assisi::Render::GpuTelemetrySample &gpu = _gpuTelemetry.Poll();
        if (gpu.valid)
        {
            ImGui::Text("%s", gpu.name.c_str());
            ImGui::Text("Clock: %u MHz core / %u MHz mem", gpu.coreClockMhz, gpu.memClockMhz);
            ImGui::Text("Util:  %u%% gpu / %u%% mem", gpu.gpuUtilPct, gpu.memUtilPct);
            if (!gpu.powerSupported)
                ImGui::Text("Power: N/A    Temp: %u C", gpu.temperatureC);
            else if (gpu.powerLimitWatts > 0.0)
                ImGui::Text("Power: %.0f / %.0f W    Temp: %u C", gpu.powerWatts, gpu.powerLimitWatts,
                            gpu.temperatureC);
            else
                ImGui::Text("Power: %.0f W    Temp: %u C", gpu.powerWatts, gpu.temperatureC);
            if (gpu.memTotalBytes > 0)
                // PRIu64, never a fixed %llu: uint64_t is `unsigned long` on Linux and
                // `unsigned long long` on Windows, so a literal is wrong on one of them.
                ImGui::Text("VRAM:  %" PRIu64 " / %" PRIu64 " MiB", gpu.memUsedBytes >> 20, gpu.memTotalBytes >> 20);

            // One point per fresh NVML reading, not per frame. Poll() is throttled, so
            // the sequence bumps ~5x/s and the graphs span ~30 s of history whatever
            // the frame rate.
            if (gpu.sequence != _lastGpuSequence)
            {
                _lastGpuSequence = gpu.sequence;
                _gpuClockHistory[static_cast<std::size_t>(_gpuTelemetryOffset)] = static_cast<float>(gpu.coreClockMhz);
                _gpuUtilHistory[static_cast<std::size_t>(_gpuTelemetryOffset)] = static_cast<float>(gpu.gpuUtilPct);
                _gpuPowerHistory[static_cast<std::size_t>(_gpuTelemetryOffset)] = static_cast<float>(gpu.powerWatts);
                _gpuTelemetryOffset = (_gpuTelemetryOffset + 1) % kGpuHistory;
                if (_gpuTelemetryCount < kGpuHistory)
                {
                    ++_gpuTelemetryCount;
                }
            }

            if (_gpuTelemetryCount > 0)
            {
                // Until the ring wraps the samples sit in [0, count) in order, so plot
                // from 0. Once it is full, the write cursor is the oldest sample, which
                // is what ImPlot's Offset wants.
                const int32_t plotCount = _gpuTelemetryCount;
                const int32_t plotOffset = _gpuTelemetryCount < kGpuHistory ? 0 : _gpuTelemetryOffset;
                const auto bufMax = [plotCount](const std::array<float, kGpuHistory> &buf)
                                    {
                                        float m = 0.0f;
                                        for (int32_t i = 0; i < plotCount; ++i)
                                        {
                                            m = std::max(m, buf[static_cast<std::size_t>(i)]);
                                        }
                                        return m;
                                    };

                // One compact history plot per metric. `title` is drawn above the plot
                // and carries the unit, which is why the y-axis label is empty; its
                // "###id" suffix keeps the ImGui id stable independent of that text.
                // Y tick labels stay on, so the scale is readable off the axis.
                const auto drawGpuPlot = [plotCount, plotOffset](const char *title,
                                                                 const std::array<float, kGpuHistory> &buf, float ymax,
                                                                 ImVec4 color)
                                         {
                                             ImPlotSpec spec;
                                             spec.LineColor = color;
                                             spec.FillColor = color;
                                             spec.FillAlpha = 0.25f;
                                             spec.LineWeight = 1.5f;
                                             spec.Offset = plotOffset;
                                             // NoInputs: the limits are re-locked every frame anyway, so pan and
                                             // zoom would do nothing except make the x-axis look like a
                                             // draggable control.
                                             if (ImPlot::BeginPlot(title, ImVec2(-1.0f, 100.0f),
                                                                   ImPlotFlags_NoMenus | ImPlotFlags_NoLegend | ImPlotFlags_NoInputs))
                                             {
                                                 ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                                                                   ImPlotAxisFlags_NoHighlight);
                                                 ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, plotCount - 1, ImPlotCond_Always);
                                                 ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, static_cast<double>(ymax), ImPlotCond_Always);
                                                 ImPlot::PlotShaded(title, buf.data(), plotCount, 0.0, 1.0, 0.0, spec);
                                                 ImPlot::PlotLine(title, buf.data(), plotCount, 1.0, 0.0, spec);
                                                 ImPlot::EndPlot();
                                             }
                                         };

                const float clockMax = std::max(bufMax(_gpuClockHistory) * 1.1f, 500.0f);
                drawGpuPlot("GPU Clock (MHz)###gpuClock", _gpuClockHistory, clockMax,
                            ImVec4(0.30f, 0.75f, 0.40f, 1.0f));
                drawGpuPlot("GPU Utilization (%)###gpuUtil", _gpuUtilHistory, 100.0f,
                            ImVec4(0.35f, 0.60f, 0.95f, 1.0f));
                // Not every GPU reports power draw — laptops often do not.
                if (gpu.powerSupported)
                {
                    const float powerMax = gpu.powerLimitWatts > 0.0 ? static_cast<float>(gpu.powerLimitWatts)
                                                                     : std::max(bufMax(_gpuPowerHistory) * 1.1f, 50.0f);
                    drawGpuPlot("GPU Power (W)###gpuPower", _gpuPowerHistory, powerMax,
                                ImVec4(0.95f, 0.55f, 0.25f, 1.0f));
                }
                else
                {
                    // A centred N/A in a framed box of exactly a plot's footprint (the
                    // same 100 px height), so the rest of the panel sits where it does
                    // on a GPU that does report power. A flat-zero line would read as a
                    // measurement.
                    if (ImGui::BeginChild("###gpuPowerNA", ImVec2(-1.0f, 100.0f), ImGuiChildFlags_Borders))
                    {
                        const ImVec2 start = ImGui::GetCursorStartPos();
                        const ImVec2 avail = ImGui::GetContentRegionAvail();

                        const char *title = "GPU Power (W)";
                        ImGui::SetCursorPosX(start.x + ((avail.x - ImGui::CalcTextSize(title).x) * 0.5f));
                        ImGui::TextUnformatted(title);

                        const char *label = "N/A (Unsupported by this GPU)";
                        const ImVec2 labelSize = ImGui::CalcTextSize(label);
                        ImGui::SetCursorPos(ImVec2(start.x + ((avail.x - labelSize.x) * 0.5f),
                                                   start.y + ((avail.y - labelSize.y) * 0.5f)));
                        ImGui::TextDisabled("%s", label);
                    }
                    ImGui::EndChild();
                }
            }
        }
        else
        {
            ImGui::TextDisabled("GPU telemetry unavailable (NVML not found)");
        }

        // CPU and GPU share one y-axis, so their heights compare directly. The top is
        // floored at 4 ms, or an idle scene would magnify sub-millisecond jitter into
        // a mountain range. Both series read the ring buffers in place, ImPlot's
        // Offset marking the chronological start.
        float plotMax = 4.0f;
        for (int32_t i = 0; i < frameHistory; ++i)
        {
            plotMax =
                std::max({plotMax, frame.cpuMs[static_cast<std::size_t>(i)], frame.gpuMs[static_cast<std::size_t>(i)]});
        }
        plotMax *= 1.1f; // headroom, so the peak is not pinned to the top edge

        // One spec per series, reused for the shaded fill and the outline so the two
        // cannot drift apart in colour.
        ImPlotSpec cpuSpec;
        cpuSpec.LineColor = ImVec4(0.95f, 0.55f, 0.25f, 1.0f); // orange
        cpuSpec.FillColor = cpuSpec.LineColor;
        cpuSpec.FillAlpha = 0.25f;
        cpuSpec.LineWeight = 1.5f;
        cpuSpec.Offset = frame.offset;

        ImPlotSpec gpuSpec;
        gpuSpec.LineColor = ImVec4(0.30f, 0.75f, 0.40f, 1.0f); // green
        gpuSpec.FillColor = gpuSpec.LineColor;
        gpuSpec.FillAlpha = 0.25f;
        gpuSpec.LineWeight = 1.5f;
        gpuSpec.Offset = frame.offset;

        if (ImPlot::BeginPlot("Frame Time (ms)###frameGraph", ImVec2(-1.0f, 120.0f),
                              ImPlotFlags_NoMenus | ImPlotFlags_NoInputs))
        {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
                              ImPlotAxisFlags_NoHighlight);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, frameHistory - 1, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, static_cast<double>(plotMax), ImPlotCond_Always);
            ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);

            ImPlot::PlotShaded("CPU", frame.cpuMs.data(), frameHistory, 0.0, 1.0, 0.0, cpuSpec);
            ImPlot::PlotLine("CPU", frame.cpuMs.data(), frameHistory, 1.0, 0.0, cpuSpec);
            ImPlot::PlotShaded("GPU", frame.gpuMs.data(), frameHistory, 0.0, 1.0, 0.0, gpuSpec);
            ImPlot::PlotLine("GPU", frame.gpuMs.data(), frameHistory, 1.0, 0.0, gpuSpec);

            ImPlot::EndPlot();
        }

        // Percentile stats over the frame-delta history. "1% low" is the average of the
        // slowest 1% of frames, which is where the stutter an average hides shows up.
        // Sorting a copy every frame is cheap at this sample count.
        if (frame.sampleCount > 0)
        {
            std::vector<float> sorted(frame.frameDeltaMs.begin(), frame.frameDeltaMs.begin() + frame.sampleCount);
            std::sort(sorted.begin(), sorted.end());

            double sum = 0.0;
            for (float ms : sorted)
            {
                sum += static_cast<double>(ms);
            }
            const double avgMs = static_cast<double>(sum) / static_cast<double>(sorted.size());

            // The tail of the sort, at least one frame however short the history is.
            const int32_t worstCount = std::max<int32_t>(1, static_cast<int32_t>(sorted.size()) / 100);
            double worstSum = 0.0;
            for (int32_t i = static_cast<int32_t>(sorted.size()) - worstCount; i < static_cast<int32_t>(sorted.size());
                 ++i)
            {
                worstSum += static_cast<double>(sorted[static_cast<std::size_t>(i)]);
            }
            const double onePctLowMs = worstSum / static_cast<double>(worstCount);

            const float minMs = sorted.front();
            const float maxMs = sorted.back();

            const auto toFps = [](double ms) { return ms > 0.0 ? static_cast<int32_t>(1000.0 / ms) : 0; };
            ImGui::Text("Avg:     %6.2f ms  (%d FPS)", avgMs, toFps(avgMs));
            ImGui::Text("1%% low:  %6.2f ms  (%d FPS)", onePctLowMs, toFps(onePctLowMs));
            ImGui::Text("Min/Max: %6.2f / %6.2f ms", static_cast<double>(minMs), static_cast<double>(maxMs));
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Rendering");

        // A/B toggle for view-frustum culling; read it against the item tally below.
        // Culling only ever removes draws, so a culled count stuck at 0 with everything
        // on screen means it is inert here and explains nothing — fly until geometry
        // leaves the view and it should climb. Runtime only, not persisted.
        bool frustumCulling = frame.renderer.FrustumCulling();
        if (ImGui::Checkbox("Frustum Culling", &frustumCulling))
        {
            frame.renderer.SetFrustumCulling(frustumCulling);
        }

        // A/B toggle for draw-list sorting. The image is identical either way, so the
        // batch tally is the tell: sorting puts identical same-material meshes next to
        // each other, where they coalesce into one instanced indirect draw and the
        // batch count falls toward the number of distinct meshes. Off, it climbs toward
        // the drawn-item count, every item its own batch.
        bool sortDraws = frame.renderer.SortDraws();
        if (ImGui::Checkbox("Sort Draws", &sortDraws))
        {
            frame.renderer.SetSortDraws(sortDraws);
        }

        // A/B toggle for the GPU-driven cull path: a compute pass culls every object and
        // builds the indirect draw commands on the GPU, coalescing identical
        // (mesh, submesh) instances, so the CPU issues one drawIndexedIndirect instead
        // of extracting and sorting a draw list. The opaque image must come out
        // identical to the CPU path — that is what this toggle is for — but for an
        // instance parked inside the LOD dead band, which the GPU does not hold. On this path the
        // tallies below are read back from the GPU and run a few frames stale, and
        // "Sort Draws" does nothing. Runtime only, not persisted to options.json.
        bool gpuCulling = frame.renderer.GpuCulling();
        if (ImGui::Checkbox("GPU Cull", &gpuCulling))
        {
            frame.renderer.SetGpuCulling(gpuCulling);
        }

        // Show or hide the editor overlays — selection outline, entity icons, collider
        // wireframes — to see the scene as a game would render it. This only skips the
        // submissions in OnRender; the scene is untouched. Whether the overlay passes
        // exist at all is a separate, Initialize-time decision
        // (EditorConfig::enableEditorVisuals, --no-editor-visuals).
        ImGui::Checkbox("Editor Overlays", &frame.showEditorOverlays);

        // Screen-size LOD selection. Off pins every instance to LOD0, which is
        // what the renderer drew before selection existed and the A/B against
        // the whole feature; the bias is the quality dial, above 1 holding a
        // finer level further out. Runtime only, not persisted.
        Assisi::Runtime::LodSettings lod = frame.renderer.LodSettings();
        bool lodChanged = ImGui::Checkbox("Mesh LOD", &lod.enabled);
        if (!lod.enabled)
        {
            ImGui::BeginDisabled();
        }
        lodChanged |= ImGui::SliderFloat("LOD Bias", &lod.bias, 0.25f, 4.f, "%.2fx");

        // Every instance on one level, for looking at a level in place instead
        // of walking backwards until it appears. The range is the deepest chain
        // the tally reports; each mesh clamps it to its own, so a level past the
        // end of a short chain shows that chain's last.
        int32_t forced = lod.forcedLevel;
        if (ImGui::SliderInt("Force LOD", &forced, -1, static_cast<int32_t>(Assisi::Runtime::kMaxReportedLods) - 1,
                             forced < 0 ? "Auto" : "LOD %d"))
        {
            lod.forcedLevel = forced;
            lodChanged = true;
        }
        ImGui::SetItemTooltip("Auto measures each instance. A level draws every instance at it, clamped to each "
                              "mesh's own chain.");
        lodChanged |= ImGui::Checkbox("Shadow LOD", &lod.shadowLod);
        ImGui::SetItemTooltip("Each sun cascade may draw a caster one level coarser than the screen does, where its "
                              "texels are too coarse to show the difference. Off casts every shadow at the "
                              "on-screen level.");
        if (!lod.enabled)
        {
            ImGui::EndDisabled();
        }
        if (lodChanged)
        {
            frame.renderer.SetLodSettings(lod);
        }

        const Assisi::Runtime::DrawStats draw = frame.renderer.LastDrawStats();
        ImGui::Text("Items: %u drawn / %u meshes culled", draw.drawnItems, draw.culledMeshes);
        ImGui::Text("Draws: %u batches / %u indirect calls", draw.batches, draw.drawCalls);

        // Instances per level, and only the levels the scene actually reached —
        // a scene of single-level meshes says "LOD0" and stops, which is what
        // "meshes with no chain are untouched" looks like from here.
        std::string levels;
        for (uint32_t level = 0; level < Assisi::Runtime::kMaxReportedLods; ++level)
        {
            if (draw.lodInstances[level] == 0)
            {
                continue;
            }
            if (!levels.empty())
            {
                levels += "  ";
            }
            levels += "LOD" + std::to_string(level) + ": " + std::to_string(draw.lodInstances[level]);
        }
        ImGui::Text("%s", levels.empty() ? "LOD: nothing drawn" : levels.c_str());

        // A pin is set on one entity in the inspector and then goes out of sight
        // the moment that entity is deselected. Said here, it cannot become a
        // mystery about why one thing on screen looks coarse.
        if (frame.renderer.HasPinnedLod())
        {
            ImGui::TextDisabled("one instance is pinned to a level (Inspector > MeshRenderer)");
        }

        // Short-circuits the mesh shader to a single material channel, to look at the
        // PBR inputs directly. Runtime only. **This list is indexed by the enum
        // value** — it must stay in Render::MaterialDebugView's order.
        static const char *kDebugViewNames[] = {"Off",    "Base Color", "Metallic", "Roughness",
                                                "Normal", "Occlusion",  "Emissive"};
        int32_t debugViewIndex = static_cast<int32_t>(frame.renderer.DebugView());
        if (ImGui::Combo("Debug View", &debugViewIndex, kDebugViewNames, IM_ARRAYSIZE(kDebugViewNames)))
        {
            frame.renderer.SetDebugView(static_cast<Assisi::Render::MaterialDebugView>(debugViewIndex));
        }

        ImGui::Separator();

        OptionsConfig &options = frame.options;

        DrawShadowSettings(frame);

        ImGui::Separator();

        DrawEnvironmentSettings(frame);

        ImGui::Separator();

        // The look. Nothing here rebuilds a target — the values ride in the tone
        // map's push constants, so every edit lands on the next frame, which is
        // what makes these usable for comparing one against another.
        //
        // **Indexed by the enum value** — this list must stay in
        // Render::TonemapOperator's order.
        static const char *kOperatorNames[] = {"AgX", "ACES", "Reinhard"};
        int32_t operatorIndex = static_cast<int32_t>(options.tonemap.op);
        if (ImGui::Combo("Tone Map", &operatorIndex, kOperatorNames, IM_ARRAYSIZE(kOperatorNames)))
        {
            options.tonemap.op = static_cast<Assisi::Render::TonemapOperator>(operatorIndex);
            options.SaveToJson();
        }

        bool lookChanged =
            ImGui::SliderFloat("Exposure", &options.tonemap.exposureStops, Assisi::Render::kMinExposureStops,
                               Assisi::Render::kMaxExposureStops, "%.2f stops");
        lookChanged |= ImGui::SliderFloat("Contrast", &options.tonemap.contrast, Assisi::Render::kMinContrast,
                                          Assisi::Render::kMaxContrast, "%.2f");
        lookChanged |= ImGui::SliderFloat("Saturation", &options.tonemap.saturation, Assisi::Render::kMinSaturation,
                                          Assisi::Render::kMaxSaturation, "%.2f");
        if (lookChanged)
        {
            options.SaveToJson();
        }

        // AgX is neutral by design and reads flat ungraded, so "no grade" is a
        // comparison point rather than a default worth returning to.
        if (ImGui::SmallButton("Punchy"))
        {
            options.tonemap.contrast = Assisi::Render::kPunchyContrast;
            options.tonemap.saturation = Assisi::Render::kPunchySaturation;
            options.SaveToJson();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Neutral"))
        {
            options.tonemap.contrast = 1.0f;
            options.tonemap.saturation = 1.0f;
            options.SaveToJson();
        }

        ImGui::Separator();

        static const char *kModeNames[] = {"Disabled", "MSAA", "FXAA", "MSAA + FXAA"};
        int modeIndex = static_cast<int>(options.aaMode);
        if (ImGui::Combo("AA Mode", &modeIndex, kModeNames, 4))
        {
            options.aaMode = static_cast<Assisi::Render::AaMode>(modeIndex);
            applyDisplay = true;
            options.SaveToJson();
        }

        const bool msaaActive =
            (options.aaMode == Assisi::Render::AaMode::MSAA || options.aaMode == Assisi::Render::AaMode::MSAA_FXAA);
        if (!msaaActive)
        {
            ImGui::BeginDisabled();
        }

        static const char *kSampleNames[] = {"2x", "4x", "8x"};
        static const int32_t kSampleValues[] = {2, 4, 8};
        int sampleIndex = 1;
        for (int32_t i = 0; i < 3; ++i)
        {
            if (kSampleValues[i] == options.msaaSamples)
            {
                sampleIndex = i;
                break;
            }
        }

        if (ImGui::Combo("MSAA Samples", &sampleIndex, kSampleNames, 3))
        {
            options.msaaSamples = kSampleValues[sampleIndex];
            if (msaaActive)
            {
                applyDisplay = true;
            }
            options.SaveToJson();
        }

        if (!msaaActive)
        {
            ImGui::EndDisabled();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Frame Sync");

        // VSync and an FPS cap are mutually exclusive, hence radio buttons. Only the
        // option is written here; Application::Run() switches the swapchain's present
        // mode between frames, which is the only safe place to do it.
        int frameSyncIndex = static_cast<int>(options.frameSync);
        bool frameSyncChanged = ImGui::RadioButton("VSync", &frameSyncIndex, static_cast<int>(FrameSyncMode::VSync));
        ImGui::SameLine();
        frameSyncChanged |= ImGui::RadioButton("FPS Limit", &frameSyncIndex, static_cast<int>(FrameSyncMode::FpsLimit));
        if (frameSyncChanged)
        {
            options.frameSync = static_cast<FrameSyncMode>(frameSyncIndex);
            options.SaveToJson();
        }

        // The cap sub-controls are live only in FpsLimit mode. "Unlimited" is the -1
        // sentinel, and greys out Max FPS in turn.
        const bool fpsMode = (options.frameSync == FrameSyncMode::FpsLimit);

        if (!fpsMode)
        {
            ImGui::BeginDisabled();
        }
        bool unlimited = (options.fpsLimit < 0);
        if (ImGui::Checkbox("Unlimited", &unlimited))
        {
            // Leaving "unlimited" seeds a usable cap rather than dropping the user into
            // an empty field.
            options.fpsLimit = unlimited ? static_cast<std::int16_t>(-1) : static_cast<std::int16_t>(60);
            options.SaveToJson();
        }
        if (!fpsMode)
        {
            ImGui::EndDisabled();
        }

        const bool capFieldEnabled = fpsMode && !unlimited;
        if (!capFieldEnabled)
        {
            ImGui::BeginDisabled();
        }
        // Commit on IsItemDeactivatedAfterEdit, not on the InputInt's return: while the
        // field is being typed in it reports every intermediate value, so "120" passes
        // through 1 and 12 and the live pacer would follow each one. `capFps` carries
        // the half-typed number until Enter, or a click or tab away.
        int capFps = (options.fpsLimit > 0) ? options.fpsLimit : 60;
        ImGui::InputInt("Max FPS", &capFps);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            // A cap of 0 or less is not a cap; the ceiling is what int16 holds.
            capFps = std::clamp(capFps, 1, static_cast<int>(INT16_MAX));
            options.fpsLimit = static_cast<std::int16_t>(capFps);
            options.SaveToJson();
        }
        if (!capFieldEnabled)
        {
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    return applyDisplay;
}

} // namespace Assisi::Editor
