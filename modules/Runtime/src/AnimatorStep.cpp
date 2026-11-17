/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AnimatorStep.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Sigil/Evaluate.hpp>
#include <Assisi/Sigil/Layout.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace Assisi::Runtime
{

namespace
{

/// Where a clip played once, forwards, holds when it has ended: AdvancePhase
/// clamps a phase that doesn't loop to the end of its 0..1 range.
constexpr float kPhaseEnd = 1.f;

/// What a weight is held between, as an AnimationLayer's is.
constexpr float kMinWeight = 0.f;
constexpr float kMaxWeight = 1.f;

/// The parts of an entity an Animator drives: what the author set, where it is
/// in its file, and what it asks playback for.
struct Driven
{
    const Animator &animator;
    AnimatorRun &run;
    AnimationPlayback &playback;
};

/// The part of the request that @p layer writes: the base for the first,
/// otherwise one of its layers.
struct Target
{
    PlaybackRequest *play = nullptr;
    float *weight = nullptr; ///< Null for the base, which has none.
};

Target TargetOf(AnimationPlayback &playback, uint32_t layer)
{
    if (layer == 0)
    {
        return Target{.play = &playback.request.base, .weight = nullptr};
    }
    LayerRequest &upper = playback.request.layers[layer - 1];
    return Target{.play = &upper.play, .weight = &upper.weight};
}

/// The track playing @p layer's animation, or null until playback has made
/// one for it.
const AnimationTrack *TrackOf(const AnimationPlayback &playback, uint32_t layer)
{
    if (layer == 0)
    {
        return &playback.track;
    }
    return layer - 1 < playback.layerStates.size() ? &playback.layerStates[layer - 1].track : nullptr;
}

float FloatAt(const AnimatorGraph &graph, std::span<const Sigil::Word> block, uint32_t entry, float otherwise)
{
    return entry == kNotWritten ? otherwise : Sigil::ToFloat(Sigil::Evaluate(graph.code, entry, block));
}

const AnimatorState &LeafOf(const AnimatorRun &run, uint32_t layer)
{
    return run.graph->layers[layer].states[run.paths[layer].back()];
}

/// What @p clip plays: the asset the file names, or the one the Animator binds
/// to its param, reported once when there is none.
Core::AssetId ClipOf(const Driven &driven, const AnimatorClip &clip)
{
    if (clip.param == kNotWritten)
    {
        return clip.asset;
    }
    const std::string &name = driven.run.graph->clipParams[clip.param];
    const std::vector<ClipBinding> &clips = driven.animator.clips;
    const std::vector<ClipBinding>::const_iterator bound =
        std::ranges::find_if(clips, [&name](const ClipBinding &binding) { return binding.name.View() == name; });
    if (bound != clips.end())
    {
        return bound->clip;
    }
    if (!driven.run.warned)
    {
        Core::Log::Warn("Animator: the file's clip param \"{}\" has no clip in the Animator's clips, so it plays "
                        "nothing.",
                        name);
        driven.run.warned = true;
    }
    return Core::AssetId{};
}

/// How a layer came to be in its state.
enum class Arrival : uint8_t
{
    Entered, ///< A transition or `then` led there.
    Rebound, ///< The file was bound or changed while it was there.
    Count_,
};

/// Asks playback for what @p layer's state plays, fading over @p fade. A
/// clip played once or held starts again from the top when entered anew, even
/// on the clip already playing; a file changing under it leaves it be.
void ApplyLeaf(const Driven &driven, uint32_t layer, float fade, Arrival arrival)
{
    const AnimatorState &leaf = LeafOf(driven.run, layer);
    const Core::AssetId clip = ClipOf(driven, leaf.clip);
    PlaybackRequest &play = *TargetOf(driven.playback, layer).play;
    const bool restarts = leaf.then != kNotWritten || leaf.pose;
    if (arrival == Arrival::Entered && play.animation == clip && restarts)
    {
        ++play.restarts;
    }
    play.startPhase = 0.f;
    play.fade = fade;
    play.fadeKind = FadeKind::CrossFade;
    play.animation = clip;
    play.loop = !restarts;
}

/// Whether @p layer's state, a clip played once, has played to its end.
bool Finished(const Driven &driven, uint32_t layer)
{
    const AnimatorState &leaf = LeafOf(driven.run, layer);
    const AnimationTrack *track = TrackOf(driven.playback, layer);
    if (leaf.then == kNotWritten || leaf.pose || track == nullptr)
    {
        return false;
    }
    // A track still on the clip before has not started this one.
    const bool playing = track->boundAnimation == ClipOf(driven, leaf.clip);
    return playing && !track->fading && track->current.Phase >= kPhaseEnd;
}

/// Writes what @p layer's state and the layer work out every frame.
void WriteOutputs(const AnimatorRun &run, AnimationPlayback &playback, uint32_t layer)
{
    const AnimatorGraph &graph = *run.graph;
    const AnimatorLayer &lowered = graph.layers[layer];
    const AnimatorState &leaf = LeafOf(run, layer);
    const Target target = TargetOf(playback, layer);
    const std::span<const Sigil::Word> block = run.block;
    target.play->speed = leaf.pose ? 0.f : FloatAt(graph, block, leaf.rate, 1.f);
    target.play->parameter = glm::vec2{FloatAt(graph, block, leaf.x, 0.f), FloatAt(graph, block, leaf.y, 0.f)};
    if (target.weight != nullptr)
    {
        *target.weight = std::clamp(FloatAt(graph, block, lowered.weight, kMaxWeight), kMinWeight, kMaxWeight);
    }
}

float LayerFade(const AnimatorRun &run, uint32_t layer)
{
    const AnimatorGraph &graph = *run.graph;
    return FloatAt(graph, run.block, graph.layers[layer].fade, kDefaultAnimatorFade);
}

void StepLayer(const Driven &driven, uint32_t layer)
{
    AnimatorRun &run = driven.run;
    const AnimatorGraph &graph = *run.graph;
    const AnimatorLayer &lowered = graph.layers[layer];
    std::vector<uint32_t> &path = run.paths[layer];
    const AnimationTrack *track = TrackOf(driven.playback, layer);
    if (graph.progressSlot != kNotWritten)
    {
        run.block[graph.progressSlot] = Sigil::FromFloat(track != nullptr ? track->current.Phase : 0.f);
    }
    Sigil::EvaluateLets(graph.code, graph.letEntries, graph.layout, run.block);

    const bool fading = track != nullptr && track->fading;
    std::vector<Sigil::Gate> &gates = run.gates;
    gates.assign(lowered.transitions.size(), Sigil::Gate::Open);
    for (std::size_t t = 0; t < lowered.transitions.size(); ++t)
    {
        gates[t] = fading && !lowered.transitions[t].interrupt ? Sigil::Gate::Closed : Sigil::Gate::Open;
    }
    const std::optional<Sigil::Fired> fired = Sigil::StepGraph(lowered.graph, path, graph.code, run.block, gates);
    if (fired.has_value())
    {
        const float fade =
            FloatAt(graph, run.block, lowered.transitions[fired->transition].fade, LayerFade(run, layer));
        Sigil::Enter(lowered.graph, fired->depth, lowered.graph.transitions[fired->transition].target, path);
        ApplyLeaf(driven, layer, fade, Arrival::Entered);
    }
    else if (Finished(driven, layer))
    {
        const uint32_t then = LeafOf(run, layer).then;
        Sigil::Enter(lowered.graph, static_cast<uint32_t>(path.size() - 2), then, path);
        ApplyLeaf(driven, layer, LayerFade(run, layer), Arrival::Entered);
    }
    WriteOutputs(run, driven.playback, layer);
}

/// The value of each param of @p previous that @p layout has too, by name
/// and type, in a block laid out by @p layout.
std::vector<Sigil::Word> CarryParams(const AnimatorRun &previous, const Sigil::Layout &layout)
{
    std::vector<Sigil::Word> block(layout.slots.size(), Sigil::Word{0});
    if (previous.graph == nullptr)
    {
        return block;
    }
    const Sigil::Layout &before = previous.graph->layout;
    for (uint32_t param = 0; param < layout.paramCount; ++param)
    {
        const Sigil::Slot &slot = layout.slots[param];
        const std::optional<uint32_t> old = Sigil::FindSlot(before, slot.name);
        const bool same = old.has_value() && before.slots[*old].kind == Sigil::SlotKind::Param &&
                          before.slots[*old].type == slot.type && *old < previous.block.size();
        if (same)
        {
            block[param] = previous.block[*old];
        }
    }
    return block;
}

/// The count each slot of @p previous last followed, for the slot of the same
/// name in a block laid out by @p layout, so a changed file does not take the
/// next count for a first sight. Only a handle that finds a trigger there reads
/// it.
std::vector<std::optional<uint32_t>> CarryCounts(const AnimatorRun &previous, const Sigil::Layout &layout)
{
    std::vector<std::optional<uint32_t>> counts(layout.slots.size());
    if (previous.graph == nullptr)
    {
        return counts;
    }
    const Sigil::Layout &before = previous.graph->layout;
    for (uint32_t old = 0; old < previous.counts.size() && old < before.slots.size(); ++old)
    {
        const std::optional<uint32_t> now = Sigil::FindSlot(layout, before.slots[old].name);
        if (previous.counts[old].has_value() && now.has_value())
        {
            counts[*now] = previous.counts[old];
        }
    }
    return counts;
}

/// The states each layer of the bound file was in, by layer name.
std::vector<std::string> PreviousNames(const AnimatorRun &previous, std::string_view layerName)
{
    if (previous.graph == nullptr)
    {
        return {};
    }
    for (std::size_t layer = 0; layer < previous.graph->layers.size(); ++layer)
    {
        if (previous.graph->layers[layer].name == layerName && layer < previous.paths.size())
        {
            return Sigil::PathNames(previous.graph->layers[layer].graph, previous.paths[layer]);
        }
    }
    return {};
}

/// Sizes the layers of @p playback's request to the file's, each with the
/// file's mask and mode.
void ShapeLayers(const AnimatorGraph &graph, AnimationPlayback &playback)
{
    playback.request.layers.resize(graph.layers.size() - 1);
    for (std::size_t layer = 1; layer < graph.layers.size(); ++layer)
    {
        LayerRequest &upper = playback.request.layers[layer - 1];
        upper.maskRoot = graph.layers[layer].maskRoot;
        upper.exclusions = graph.layers[layer].exclusions;
        upper.mode = graph.layers[layer].mode;
        // The file's weight is worked out every frame, and eases if the file says so.
        upper.weightFade = 0.f;
    }
}

} // namespace

void BindAnimator(const Animator &animator, AnimatorRun &run, AnimationPlayback &playback,
                  std::shared_ptr<const AnimatorGraph> graph)
{
    // A library has no layers, and plays nothing; it never loads as a graph.
    if (graph == nullptr || graph->layers.empty())
    {
        return;
    }
    AnimatorRun bound;
    bound.block = CarryParams(run, graph->layout);
    bound.counts = CarryCounts(run, graph->layout);
    bound.paths.resize(graph->layers.size());
    for (std::size_t layer = 0; layer < graph->layers.size(); ++layer)
    {
        Sigil::Remap(graph->layers[layer].graph, PreviousNames(run, graph->layers[layer].name), bound.paths[layer]);
    }
    bound.graph = std::move(graph);
    run = std::move(bound);
    ShapeLayers(*run.graph, playback);
    const Driven driven{.animator = animator, .run = run, .playback = playback};
    for (uint32_t layer = 0; layer < run.graph->layers.size(); ++layer)
    {
        ApplyLeaf(driven, layer, LayerFade(run, layer), Arrival::Rebound);
    }
}

void StepAnimator(const Animator &animator, AnimatorRun &run, AnimationPlayback &playback)
{
    if (run.graph == nullptr)
    {
        return;
    }
    // A request whose layers were changed from outside is shaped back to the file's.
    if (playback.request.layers.size() + 1 != run.graph->layers.size())
    {
        ShapeLayers(*run.graph, playback);
    }
    const Driven driven{.animator = animator, .run = run, .playback = playback};
    for (uint32_t layer = 0; layer < run.graph->layers.size(); ++layer)
    {
        StepLayer(driven, layer);
    }
    for (const uint32_t slot : run.graph->triggerSlots)
    {
        run.block[slot] = Sigil::FromBool(false);
    }
}

std::vector<std::string> AnimatorStateNames(const AnimatorRun &run, uint32_t layer)
{
    if (run.graph == nullptr || layer >= run.paths.size())
    {
        return {};
    }
    return Sigil::PathNames(run.graph->layers[layer].graph, run.paths[layer]);
}

} // namespace Assisi::Runtime
