/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AnimationPlayback.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/AnimationBlend.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>
#include <Assisi/Geometry/Pose.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Assisi::Runtime
{

namespace
{

using ClipList = std::span<const std::shared_ptr<const Geometry::AnimationClip>>;
using Pose = std::span<Geometry::JointTransform>;

/// How a track's frame went.
enum class TrackStep : uint8_t
{
    Waiting, ///< Its clips or the mesh aren't ready, so nothing was written.
    Played,
    Rebound, ///< Played, after binding to new clips or a new mesh.
    Count_,
};

bool SameClips(const Geometry::ClipBlend &blend, ClipList clips)
{
    if (blend.Sources.size() != clips.size())
    {
        return false;
    }
    for (std::size_t source = 0; source < clips.size(); ++source)
    {
        if (blend.Sources[source].Clip != clips[source])
        {
            return false;
        }
    }
    return true;
}

bool NeedsRebind(const AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation)
{
    return settings.animation != track.boundAnimation || settings.meshId != track.boundMeshId ||
           animation.space != track.space || !SameClips(track.current, animation.clips);
}

/// Whatever was fading out stops, so only the current animation plays.
void EndFade(AnimationTrack &track)
{
    track.fading = false;
    track.outgoing.Sources.clear();
    track.outgoing.Weights.clear();
}

/// Fades from the pose as it is. A fade already running is frozen where it
/// got to, so the new fade starts from what was on screen.
void StartFade(AnimationTrack &track, const TrackSettings &settings, std::span<const Geometry::JointTransform> pose)
{
    track.fromPose.assign(pose.begin(), pose.end());
    if (track.fading)
    {
        track.outgoing.Sources.clear();
        track.outgoing.Weights.clear();
    }
    else
    {
        // Swapped rather than copied, so the next animation reuses the buffers.
        std::swap(track.outgoing, track.current);
    }
    track.toPose.assign(settings.underneath.begin(), settings.underneath.end());
    track.crossFade = Geometry::CrossFade{.Elapsed = 0.f, .Duration = settings.fade};
    track.fading = true;
}

void BindClips(Geometry::ClipBlend &blend, ClipList clips, const Geometry::Skeleton &skeleton)
{
    blend.Sources.resize(clips.size());
    for (std::size_t source = 0; source < clips.size(); ++source)
    {
        blend.Sources[source].Clip = clips[source];
        blend.Sources[source].Binding = Geometry::BindClip(*clips[source], skeleton);
    }
    // A clip on its own has all the weight; a space's come from its parameter.
    blend.Weights.assign(clips.size(), clips.size() == 1 ? 1.f : 0.f);
}

void TakePositions(AnimationTrack &track, const Geometry::BlendSpace *space)
{
    track.positions.clear();
    if (space == nullptr)
    {
        return;
    }
    for (const Geometry::BlendPoint &point : space->Points)
    {
        track.positions.push_back(point.Position);
    }
}

void Rebind(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation,
            const Geometry::Skeleton &skeleton, Pose out)
{
    const bool changed = settings.animation != track.boundAnimation;
    // Fading needs a pose to fade from that is this mesh's.
    const bool fades = changed && settings.fade > 0.f && settings.meshId == track.boundMeshId;
    if (fades)
    {
        StartFade(track, settings, out);
    }
    else
    {
        EndFade(track);
        // An animation only moves the joints it has tracks for. Without this,
        // joints one played before this one moved would stay where it left them.
        std::ranges::copy(settings.underneath, out.begin());
    }

    BindClips(track.current, animation.clips, skeleton);
    if (changed)
    {
        track.current.Phase = 0.f;
    }
    track.space = animation.space;
    TakePositions(track, animation.space.get());
    track.boundAnimation = settings.animation;
    track.boundMeshId = settings.meshId;
    track.warned = false;
}

void Fade(AnimationTrack &track, const TrackSettings &settings, const Geometry::Skeleton &skeleton, float step,
          Pose out)
{
    if (!track.outgoing.Sources.empty())
    {
        Geometry::AdvancePhase(track.outgoing, step, settings.loop);
        Geometry::SampleBlend(track.outgoing, skeleton, track.fromPose);
    }
    std::ranges::copy(settings.underneath, track.toPose.begin());
    Geometry::SampleBlend(track.current, skeleton, track.toPose);
    track.crossFade.Advance(settings.dt);
    Geometry::MixPoses(track.fromPose, track.toPose, track.crossFade.Weight(), out);
    if (track.crossFade.Done())
    {
        std::ranges::copy(track.toPose, out.begin());
        EndFade(track);
    }
}

TrackStep Step(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation,
               const Geometry::Skeleton &skeleton, Pose out)
{
    // No animation is no clips; an animation with none has not loaded yet.
    const bool loaded = settings.animation.IsNil() || !animation.clips.empty();
    if (!loaded || std::ranges::contains(animation.clips, nullptr) || out.size() != skeleton.JointCount())
    {
        return TrackStep::Waiting;
    }
    ASSISI_ASSERT(animation.space == nullptr || animation.space->Points.size() == animation.clips.size(),
                  "a clip per point");
    ASSISI_ASSERT(settings.underneath.size() == out.size(), "what lies underneath is a whole pose");

    const bool rebind = NeedsRebind(track, settings, animation);
    if (rebind)
    {
        Rebind(track, settings, animation, skeleton, out);
    }

    if (track.space != nullptr)
    {
        Geometry::BlendWeights(track.positions, settings.parameter, track.current.Weights);
    }
    // The timeline follows `speed`; a fade's seconds do not.
    const float step = settings.dt * settings.speed;
    Geometry::AdvancePhase(track.current, step, settings.loop);
    if (track.fading)
    {
        Fade(track, settings, skeleton, step, out);
    }
    else
    {
        if (settings.refill)
        {
            std::ranges::copy(settings.underneath, out.begin());
        }
        Geometry::SampleBlend(track.current, skeleton, out);
    }
    return rebind ? TrackStep::Rebound : TrackStep::Played;
}

/// Climbs from @p joint towards the root: taken once it meets @p root, not
/// once it meets an excluded joint, whichever comes first. With no root, any
/// joint not under an exclusion is taken.
uint8_t Taken(const Geometry::Skeleton &skeleton, int32_t joint, int32_t root, const std::vector<uint8_t> &excluded)
{
    // A parent chain longer than the skeleton is a loop, which takes nothing.
    for (uint32_t climbed = 0; climbed <= skeleton.JointCount(); ++climbed)
    {
        if (joint == Geometry::kNoParent)
        {
            return root == Geometry::kNoJoint ? 1 : 0;
        }
        if (excluded[static_cast<std::size_t>(joint)] != 0)
        {
            return 0;
        }
        if (joint == root)
        {
            return 1;
        }
        joint = skeleton.Parents[static_cast<std::size_t>(joint)];
    }
    return 0;
}

/// Moves the layer's weight on towards its own, starting a new slide when
/// that changed. The first frame takes it as it is.
void SlideWeight(const AnimationLayer &layer, LayerState &state, float dt)
{
    if (!state.started)
    {
        state.weight = layer.weight;
        state.fromWeight = layer.weight;
        state.toWeight = layer.weight;
        state.weightChange = Geometry::CrossFade{};
        state.started = true;
        return;
    }
    if (layer.weight != state.toWeight)
    {
        state.fromWeight = state.weight;
        state.toWeight = layer.weight;
        const float seconds = layer.weightSlide == WeightSlide::Speed
                                  ? layer.weightFade * std::abs(state.toWeight - state.fromWeight)
                                  : layer.weightFade;
        state.weightChange = Geometry::CrossFade{.Elapsed = 0.f, .Duration = seconds};
    }
    state.weightChange.Advance(dt);
    state.weight = std::lerp(state.fromWeight, state.toWeight, state.weightChange.Weight());
}

/// Builds the layer's mask again when it or the mesh changed.
void RefreshMask(const AnimationLayer &layer, LayerState &state, const Geometry::Skeleton &skeleton, uint32_t meshId)
{
    if (state.maskMeshId == meshId && state.maskRoot == layer.maskRoot && state.maskExclusions == layer.exclusions)
    {
        return;
    }
    const MaskResult result = BuildJointMask(skeleton, layer.maskRoot, layer.exclusions, state.mask);
    state.maskRoot = layer.maskRoot;
    state.maskExclusions = layer.exclusions;
    state.maskMeshId = meshId;
    state.rootMissing = result.rootMissing;
    state.exclusionMissing = result.exclusionMissing;
    state.maskWarned = false;
}

/// Works out the first frame an additive layer measures its change from. While
/// its animation fades, that is the mix of the outgoing first frame and the
/// incoming one, so the change it adds doesn't jump.
void UpdateReference(LayerState &state, const Geometry::Skeleton &skeleton, bool fadeStarted)
{
    if (state.mixedReference.size() != skeleton.JointCount())
    {
        state.mixedReference = skeleton.RestLocal;
    }
    if (fadeStarted)
    {
        state.fromReference = state.mixedReference;
    }
    state.reference = skeleton.RestLocal;
    Geometry::SampleBlendAt(state.track.current, 0.f, skeleton, state.reference);
    if (state.track.fading)
    {
        Geometry::MixPoses(state.fromReference, state.reference, state.track.crossFade.Weight(),
                           state.mixedReference);
    }
    else
    {
        state.mixedReference = state.reference;
    }
}

void Override(const LayerState &state, Pose pose)
{
    for (std::size_t joint = 0; joint < pose.size(); ++joint)
    {
        if (state.mask[joint] != 0)
        {
            pose[joint] = Geometry::MixJoint(pose[joint], state.pose[joint], state.weight);
        }
    }
}

void Add(const LayerState &state, Pose pose)
{
    for (std::size_t joint = 0; joint < pose.size(); ++joint)
    {
        if (state.mask[joint] != 0)
        {
            Geometry::AddJoint(pose[joint], state.pose[joint], state.mixedReference[joint], state.weight);
        }
    }
}

bool Belongs(const LayerState &state, const AnimationLayer &layer)
{
    return state.layerAnimation == layer.animation && state.layerMode == layer.mode;
}

bool Aligned(const AnimationPlayer &player)
{
    if (player.layerStates.size() != player.layers.size())
    {
        return false;
    }
    for (std::size_t layer = 0; layer < player.layers.size(); ++layer)
    {
        if (!Belongs(player.layerStates[layer], player.layers[layer]))
        {
            return false;
        }
    }
    return true;
}

/// Moves into each place @p placed has not filled the first state of @p old
/// not yet @p taken that @p fits it.
template <typename Fits>
void Place(AnimationPlayer &player, std::vector<LayerState> &old, std::vector<uint8_t> &taken,
           std::vector<uint8_t> &placed, Fits fits)
{
    for (std::size_t layer = 0; layer < player.layers.size(); ++layer)
    {
        for (std::size_t candidate = 0; candidate < old.size() && placed[layer] == 0; ++candidate)
        {
            if (taken[candidate] == 0 && fits(layer, candidate))
            {
                player.layerStates[layer] = std::move(old[candidate]);
                taken[candidate] = 1;
                placed[layer] = 1;
            }
        }
    }
}

/// Puts back, as the base left them last frame, the joints any layer's mask
/// took then. The states are last frame's, read before the layers run again.
void RestoreUnderLayers(const AnimationPlayer &player, Pose pose)
{
    if (player.underLayers.size() != pose.size())
    {
        return;
    }
    for (const LayerState &state : player.layerStates)
    {
        if (state.mask.size() != pose.size())
        {
            continue;
        }
        for (std::size_t joint = 0; joint < pose.size(); ++joint)
        {
            if (state.mask[joint] != 0)
            {
                pose[joint] = player.underLayers[joint];
            }
        }
    }
}

} // namespace

bool AdvanceTrack(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation,
                  const Geometry::Skeleton &skeleton, std::span<Geometry::JointTransform> out)
{
    return Step(track, settings, animation, skeleton, out) == TrackStep::Rebound;
}

bool AdvanceAnimationPlayer(AnimationPlayer &player, const ResolvedAnimation &animation,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned)
{
    if (skinned.boundMeshId == kUnboundMesh || skinned.pose.size() != skeleton.JointCount())
    {
        return false;
    }
    RestoreUnderLayers(player, skinned.pose);
    const TrackSettings settings{.underneath = skeleton.RestLocal,
                                 .animation = player.animation,
                                 .parameter = player.parameter,
                                 .speed = player.speed,
                                 .fade = player.fade,
                                 .dt = dt,
                                 .meshId = skinned.boundMeshId,
                                 .loop = player.loop,
                                 .refill = false};
    const bool rebound = AdvanceTrack(player.track, settings, animation, skeleton, skinned.pose);
    if (!player.layers.empty())
    {
        player.underLayers.assign(skinned.pose.begin(), skinned.pose.end());
    }
    return rebound;
}

void MatchLayerStates(AnimationPlayer &player)
{
    if (Aligned(player))
    {
        return;
    }
    std::vector<LayerState> old = std::move(player.layerStates);
    player.layerStates.clear();
    player.layerStates.resize(player.layers.size());
    std::vector<uint8_t> taken(old.size(), 0);
    std::vector<uint8_t> placed(player.layers.size(), 0);
    // Where it was, then where its layer moved to, then, for a layer that
    // changed, the state at its own place.
    Place(player, old, taken, placed, [&](std::size_t layer, std::size_t candidate) {
            return layer == candidate && Belongs(old[candidate], player.layers[layer]);
        });
    Place(player, old, taken, placed, [&](std::size_t layer, std::size_t candidate) {
            return Belongs(old[candidate], player.layers[layer]);
        });
    Place(player, old, taken, placed, [](std::size_t layer, std::size_t candidate) { return layer == candidate; });
    for (std::size_t layer = 0; layer < player.layers.size(); ++layer)
    {
        player.layerStates[layer].layerAnimation = player.layers[layer].animation;
        player.layerStates[layer].layerMode = player.layers[layer].mode;
    }
}

MaskResult BuildJointMask(const Geometry::Skeleton &skeleton, Core::InternedString root,
                          std::span<const Core::InternedString> exclusions, std::vector<uint8_t> &out)
{
    MaskResult result;
    const std::size_t joints = skeleton.JointCount();
    out.assign(joints, 0);
    std::vector<uint8_t> excluded(joints, 0);
    for (const Core::InternedString &exclusion : exclusions)
    {
        if (exclusion.View().empty())
        {
            continue;
        }
        const int32_t joint = Geometry::FindJoint(skeleton, exclusion.View());
        if (joint == Geometry::kNoJoint)
        {
            result.exclusionMissing = true;
            continue;
        }
        excluded[static_cast<std::size_t>(joint)] = 1;
    }
    int32_t rootJoint = Geometry::kNoJoint;
    if (!root.View().empty())
    {
        rootJoint = Geometry::FindJoint(skeleton, root.View());
        if (rootJoint == Geometry::kNoJoint)
        {
            result.rootMissing = true;
            return result;
        }
    }
    for (std::size_t joint = 0; joint < joints; ++joint)
    {
        out[joint] = Taken(skeleton, static_cast<int32_t>(joint), rootJoint, excluded);
    }
    return result;
}

void AdvanceAnimationLayer(const AnimationLayer &layer, LayerState &state, const ResolvedAnimation &animation,
                           const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned)
{
    if (skinned.boundMeshId == kUnboundMesh || skinned.pose.size() != skeleton.JointCount())
    {
        return;
    }
    SlideWeight(layer, state, dt);
    RefreshMask(layer, state, skeleton, skinned.boundMeshId);

    // An additive layer measures from rest, so a joint its animation doesn't
    // move adds nothing; an override one leaves such a joint as it is below.
    const bool additive = layer.mode == LayerMode::Additive;
    const std::span<const Geometry::JointTransform> underneath =
        additive ? std::span<const Geometry::JointTransform>{skeleton.RestLocal}
                 : std::span<const Geometry::JointTransform>{skinned.pose};
    if (state.pose.size() != skeleton.JointCount())
    {
        state.pose.assign(underneath.begin(), underneath.end());
    }
    const TrackSettings settings{.underneath = underneath,
                                 .animation = layer.animation,
                                 .parameter = layer.parameter,
                                 .speed = layer.speed,
                                 .fade = layer.fade,
                                 .dt = dt,
                                 .meshId = skinned.boundMeshId,
                                 .loop = layer.loop,
                                 .refill = true};
    const TrackStep step = Step(state.track, settings, animation, skeleton, state.pose);
    if (step == TrackStep::Waiting)
    {
        return;
    }
    if (additive)
    {
        UpdateReference(state, skeleton, step == TrackStep::Rebound && state.track.fading);
    }
    if (state.weight <= 0.f)
    {
        return;
    }
    if (additive)
    {
        Add(state, skinned.pose);
    }
    else
    {
        Override(state, skinned.pose);
    }
}

} // namespace Assisi::Runtime
