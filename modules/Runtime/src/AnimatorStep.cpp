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

/// The settings of the player that @p layer writes: the base for the first,
/// otherwise one of its layers.
struct Target
{
    Core::AssetId *animation = nullptr;
    glm::vec2 *parameter = nullptr;
    float *speed = nullptr;
    float *fade = nullptr;
    float *weight = nullptr; ///< Null for the base, which has none.
    bool *loop = nullptr;
};

Target TargetOf(AnimationPlayer &player, uint32_t layer)
{
    if (layer == 0)
    {
        return Target{.animation = &player.animation,
                      .parameter = &player.parameter,
                      .speed = &player.speed,
                      .fade = &player.fade,
                      .weight = nullptr,
                      .loop = &player.loop};
    }
    AnimationLayer &upper = player.layers[layer - 1];
    return Target{.animation = &upper.animation,
                  .parameter = &upper.parameter,
                  .speed = &upper.speed,
                  .fade = &upper.fade,
                  .weight = &upper.weight,
                  .loop = &upper.loop};
}

/// The track playing @p layer's animation, or null until the player has made
/// one for it.
AnimationTrack *TrackOf(AnimationPlayer &player, uint32_t layer)
{
    if (layer == 0)
    {
        return &player.track;
    }
    return layer - 1 < player.layerStates.size() ? &player.layerStates[layer - 1].track : nullptr;
}

float FloatAt(const AnimatorGraph &graph, std::span<const Sigil::Word> block, uint32_t entry, float otherwise)
{
    return entry == kNotWritten ? otherwise : Sigil::ToFloat(Sigil::Evaluate(graph.code, entry, block));
}

const AnimatorState &LeafOf(const Animator &animator, uint32_t layer)
{
    return animator.run.graph->layers[layer].states[animator.run.paths[layer].back()];
}

/// What @p clip plays: the asset the file names, or the one the Animator binds
/// to its param, reported once when there is none.
Core::AssetId ClipOf(Animator &animator, const AnimatorClip &clip)
{
    if (clip.param == kNotWritten)
    {
        return clip.asset;
    }
    const std::string &name = animator.run.graph->clipParams[clip.param];
    const std::vector<ClipBinding>::const_iterator bound = std::ranges::find_if(
        animator.clips, [&name](const ClipBinding &binding) { return binding.name.View() == name; });
    if (bound != animator.clips.end())
    {
        return bound->clip;
    }
    if (!animator.run.warned)
    {
        Core::Log::Warn("Animator: the file's clip param \"{}\" has no clip in the Animator's clips, so it plays "
                        "nothing.",
                        name);
        animator.run.warned = true;
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

/// Puts @p player on what @p layer's state plays, fading over @p fade. A
/// clip played once or held starts again from the top when entered anew, even
/// on the clip already playing; a file changing under it leaves it be.
void ApplyLeaf(Animator &animator, AnimationPlayer &player, uint32_t layer, float fade, Arrival arrival)
{
    const AnimatorState &leaf = LeafOf(animator, layer);
    const Core::AssetId clip = ClipOf(animator, leaf.clip);
    const Target target = TargetOf(player, layer);
    AnimationTrack *track = TrackOf(player, layer);
    const bool restarts = leaf.then != kNotWritten || leaf.pose;
    if (arrival == Arrival::Entered && *target.animation == clip && restarts && track != nullptr)
    {
        track->current.Phase = 0.f;
    }
    *target.fade = fade;
    *target.animation = clip;
    *target.loop = !restarts;
}

/// Whether @p layer's state, a clip played once, has played to its end.
bool Finished(Animator &animator, AnimationPlayer &player, uint32_t layer)
{
    const AnimatorState &leaf = LeafOf(animator, layer);
    const AnimationTrack *track = TrackOf(player, layer);
    if (leaf.then == kNotWritten || leaf.pose || track == nullptr)
    {
        return false;
    }
    // A track still on the clip before has not started this one.
    const bool playing = track->boundAnimation == ClipOf(animator, leaf.clip);
    return playing && !track->fading && track->current.Phase >= kPhaseEnd;
}

/// Writes what @p layer's state and the layer work out every frame.
void WriteOutputs(Animator &animator, AnimationPlayer &player, uint32_t layer)
{
    const AnimatorGraph &graph = *animator.run.graph;
    const AnimatorLayer &lowered = graph.layers[layer];
    const AnimatorState &leaf = LeafOf(animator, layer);
    const Target target = TargetOf(player, layer);
    const std::span<const Sigil::Word> block = animator.run.block;
    *target.speed = leaf.pose ? 0.f : FloatAt(graph, block, leaf.rate, 1.f);
    *target.parameter = glm::vec2{FloatAt(graph, block, leaf.x, 0.f), FloatAt(graph, block, leaf.y, 0.f)};
    if (target.weight != nullptr)
    {
        *target.weight = std::clamp(FloatAt(graph, block, lowered.weight, kMaxWeight), kMinWeight, kMaxWeight);
    }
}

float LayerFade(const Animator &animator, uint32_t layer)
{
    const AnimatorGraph &graph = *animator.run.graph;
    return FloatAt(graph, animator.run.block, graph.layers[layer].fade, kDefaultAnimatorFade);
}

void StepLayer(Animator &animator, AnimationPlayer &player, uint32_t layer)
{
    const AnimatorGraph &graph = *animator.run.graph;
    const AnimatorLayer &lowered = graph.layers[layer];
    std::vector<uint32_t> &path = animator.run.paths[layer];
    const AnimationTrack *track = TrackOf(player, layer);
    if (graph.progressSlot != kNotWritten)
    {
        animator.run.block[graph.progressSlot] = Sigil::FromFloat(track != nullptr ? track->current.Phase : 0.f);
    }
    Sigil::EvaluateLets(graph.code, graph.letEntries, graph.layout, animator.run.block);

    const bool fading = track != nullptr && track->fading;
    std::vector<Sigil::Gate> &gates = animator.run.gates;
    gates.assign(lowered.transitions.size(), Sigil::Gate::Open);
    for (std::size_t t = 0; t < lowered.transitions.size(); ++t)
    {
        gates[t] = fading && !lowered.transitions[t].interrupt ? Sigil::Gate::Closed : Sigil::Gate::Open;
    }
    const std::optional<Sigil::Fired> fired =
        Sigil::StepGraph(lowered.graph, path, graph.code, animator.run.block, gates);
    if (fired.has_value())
    {
        const float fade =
            FloatAt(graph, animator.run.block, lowered.transitions[fired->transition].fade, LayerFade(animator, layer));
        Sigil::Enter(lowered.graph, fired->depth, lowered.graph.transitions[fired->transition].target, path);
        ApplyLeaf(animator, player, layer, fade, Arrival::Entered);
    }
    else if (Finished(animator, player, layer))
    {
        const uint32_t then = LeafOf(animator, layer).then;
        Sigil::Enter(lowered.graph, static_cast<uint32_t>(path.size() - 2), then, path);
        ApplyLeaf(animator, player, layer, LayerFade(animator, layer), Arrival::Entered);
    }
    WriteOutputs(animator, player, layer);
}

/// The param slot named @p name holding @p type, if the bound file has one.
std::optional<uint32_t> ParamSlot(const Animator &animator, std::string_view name, Sigil::SlotType type)
{
    if (animator.run.graph == nullptr)
    {
        return std::nullopt;
    }
    const Sigil::Layout &layout = animator.run.graph->layout;
    const std::optional<uint32_t> slot = Sigil::FindSlot(layout, name);
    if (!slot.has_value() || layout.slots[*slot].kind != Sigil::SlotKind::Param || layout.slots[*slot].type != type)
    {
        return std::nullopt;
    }
    return slot;
}

bool IsTrigger(const Animator &animator, uint32_t slot)
{
    return std::ranges::find(animator.run.graph->triggerSlots, slot) != animator.run.graph->triggerSlots.end();
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

/// Sizes @p player's layers to the file's, each with the file's mask and mode.
void ShapeLayers(const AnimatorGraph &graph, AnimationPlayer &player)
{
    player.layers.resize(graph.layers.size() - 1);
    for (std::size_t layer = 1; layer < graph.layers.size(); ++layer)
    {
        AnimationLayer &upper = player.layers[layer - 1];
        upper.maskRoot = graph.layers[layer].maskRoot;
        upper.exclusions = graph.layers[layer].exclusions;
        upper.mode = graph.layers[layer].mode;
        // The file's weight is worked out every frame, and eases if the file says so.
        upper.weightFade = 0.f;
    }
}

} // namespace

void BindAnimator(Animator &animator, AnimationPlayer &player, std::shared_ptr<const AnimatorGraph> graph)
{
    // A library has no layers, and plays nothing; it never loads as a graph.
    if (graph == nullptr || graph->layers.empty())
    {
        return;
    }
    AnimatorRun run;
    run.block = CarryParams(animator.run, graph->layout);
    run.paths.resize(graph->layers.size());
    for (std::size_t layer = 0; layer < graph->layers.size(); ++layer)
    {
        Sigil::Remap(graph->layers[layer].graph, PreviousNames(animator.run, graph->layers[layer].name),
                     run.paths[layer]);
    }
    run.graph = std::move(graph);
    animator.run = std::move(run);
    ShapeLayers(*animator.run.graph, player);
    for (uint32_t layer = 0; layer < animator.run.graph->layers.size(); ++layer)
    {
        ApplyLeaf(animator, player, layer, LayerFade(animator, layer), Arrival::Rebound);
    }
}

void StepAnimator(Animator &animator, AnimationPlayer &player)
{
    if (animator.run.graph == nullptr)
    {
        return;
    }
    // A player whose layers were edited away is shaped back to the file's.
    if (player.layers.size() + 1 != animator.run.graph->layers.size())
    {
        ShapeLayers(*animator.run.graph, player);
    }
    for (uint32_t layer = 0; layer < animator.run.graph->layers.size(); ++layer)
    {
        StepLayer(animator, player, layer);
    }
    for (const uint32_t slot : animator.run.graph->triggerSlots)
    {
        animator.run.block[slot] = Sigil::FromBool(false);
    }
}

std::vector<std::string> AnimatorStateNames(const Animator &animator, uint32_t layer)
{
    if (animator.run.graph == nullptr || layer >= animator.run.paths.size())
    {
        return {};
    }
    return Sigil::PathNames(animator.run.graph->layers[layer].graph, animator.run.paths[layer]);
}

bool SetAnimatorFloat(Animator &animator, std::string_view name, float value)
{
    const std::optional<uint32_t> slot = ParamSlot(animator, name, Sigil::SlotType::Float);
    if (!slot.has_value())
    {
        return false;
    }
    animator.run.block[*slot] = Sigil::FromFloat(value);
    return true;
}

bool SetAnimatorInt(Animator &animator, std::string_view name, int32_t value)
{
    const std::optional<uint32_t> slot = ParamSlot(animator, name, Sigil::SlotType::Int);
    if (!slot.has_value())
    {
        return false;
    }
    animator.run.block[*slot] = Sigil::FromInt(value);
    return true;
}

bool SetAnimatorBool(Animator &animator, std::string_view name, bool value)
{
    const std::optional<uint32_t> slot = ParamSlot(animator, name, Sigil::SlotType::Bool);
    if (!slot.has_value() || IsTrigger(animator, *slot))
    {
        return false;
    }
    animator.run.block[*slot] = Sigil::FromBool(value);
    return true;
}

bool FireAnimatorTrigger(Animator &animator, std::string_view name)
{
    const std::optional<uint32_t> slot = ParamSlot(animator, name, Sigil::SlotType::Bool);
    if (!slot.has_value() || !IsTrigger(animator, *slot))
    {
        return false;
    }
    animator.run.block[*slot] = Sigil::FromBool(true);
    return true;
}

} // namespace Assisi::Runtime
