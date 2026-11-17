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

bool NewClips(const AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation)
{
    return settings.animation != track.boundAnimation || animation.space != track.space ||
           !SameClips(track.current, animation.clips);
}

/// Whatever was fading out stops, so only the current animation plays.
void EndFade(AnimationTrack &track)
{
    track.fading = false;
    track.snapshotPending = false;
    track.outgoing.Sources.clear();
    track.outgoing.Weights.clear();
}

/// Fades from the pose as it is, which the next sampling takes. A fade already
/// running is frozen where it got to, so the new fade starts from what was on
/// screen.
void StartFade(AnimationTrack &track, const TrackSettings &settings)
{
    if (track.fading)
    {
        track.outgoing.Sources.clear();
        track.outgoing.Weights.clear();
    }
    else
    {
        // Swapped rather than copied, so the next animation reuses the buffers.
        std::swap(track.outgoing, track.current);
        // It goes on as it was playing, not as the state it fades into asks.
        track.outgoingSpeed = track.speed;
        track.outgoingLoop = track.loop;
    }
    track.crossFade = Geometry::CrossFade{.Elapsed = 0.f, .Duration = settings.fade};
    track.fading = true;
    track.snapshotPending = true;
}

/// Puts @p clips in @p blend, unbound until a skeleton is known.
void TakeClips(Geometry::ClipBlend &blend, ClipList clips)
{
    blend.Sources.resize(clips.size());
    for (std::size_t source = 0; source < clips.size(); ++source)
    {
        blend.Sources[source].Clip = clips[source];
        blend.Sources[source].Binding = Geometry::ClipBinding{};
    }
    // A clip on its own has all the weight; a space's come from its parameter.
    blend.Weights.assign(clips.size(), clips.size() == 1 ? 1.f : 0.f);
}

void BindClips(Geometry::ClipBlend &blend, const Geometry::Skeleton &skeleton)
{
    for (Geometry::BlendSource &source : blend.Sources)
    {
        source.Binding = Geometry::BindClip(*source.Clip, skeleton);
    }
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

/// Takes the animation's clips when they are new: a different animation fades
/// in over the fade, from the second one on, and starts at the start phase;
/// anything else cuts. Needs no skeleton.
void StartClips(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation)
{
    const bool changed = settings.animation != track.boundAnimation;
    if (changed && settings.fade > 0.f && track.started)
    {
        StartFade(track, settings);
    }
    else
    {
        EndFade(track);
        // An animation only moves the joints it has tracks for. Without this,
        // joints one played before this one moved would stay where it left them.
        track.refillPending = true;
    }

    TakeClips(track.current, animation.clips);
    track.bindingsStale = true;
    if (changed)
    {
        track.current.Phase = settings.startPhase;
    }
    track.space = animation.space;
    TakePositions(track, animation.space.get());
    track.boundAnimation = settings.animation;
}

/// Moves @p track on by the frame: which clips play, their weights and phases,
/// and the fade's clock. Needs no skeleton, so a process that poses nothing
/// can still run an animator through its states.
TrackStep AdvanceClips(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation)
{
    // No animation is no clips; an animation with none has not loaded yet.
    const bool loaded = settings.animation.IsNil() || !animation.clips.empty();
    track.waiting = !loaded || std::ranges::contains(animation.clips, nullptr);
    if (track.waiting)
    {
        return TrackStep::Waiting;
    }
    ASSISI_ASSERT(animation.space == nullptr || animation.space->Points.size() == animation.clips.size(),
                  "a clip per point");

    const bool rebound = NewClips(track, settings, animation);
    if (rebound)
    {
        StartClips(track, settings, animation);
    }
    // Playing no animation counts: what code sets after it fades in from the
    // pose as it is, rather than being taken as the first and cutting in.
    track.started = true;
    // Asked to start again: what is playing goes back to the start phase, and
    // nothing fades, since the pose on screen is the same animation's.
    if (settings.restarts != track.restarts)
    {
        track.current.Phase = settings.startPhase;
        track.restarts = settings.restarts;
    }

    if (track.space != nullptr)
    {
        Geometry::BlendWeights(track.positions, settings.parameter, track.current.Weights);
    }
    // The timeline follows `speed`; a fade's seconds do not.
    Geometry::AdvancePhase(track.current, settings.dt * settings.speed, settings.loop);
    track.speed = settings.speed;
    track.loop = settings.loop;
    if (track.fading)
    {
        if (!track.outgoing.Sources.empty())
        {
            Geometry::AdvancePhase(track.outgoing, settings.dt * track.outgoingSpeed, track.outgoingLoop);
        }
        track.crossFade.Advance(settings.dt);
        if (track.crossFade.Done())
        {
            // The pose lands exactly on the new animation, as a cut would.
            EndFade(track);
            track.refillPending = true;
        }
    }
    return rebound ? TrackStep::Rebound : TrackStep::Played;
}

/// Writes what @p track plays into @p out: binds its clips to @p skeleton when
/// they or the mesh are new, then samples and blends. Returns Rebound when it
/// bound, Waiting when the track has nothing to play yet.
TrackStep SampleClips(AnimationTrack &track, const TrackSettings &settings, const Geometry::Skeleton &skeleton,
                      Pose out)
{
    if (track.waiting || !track.started || out.size() != skeleton.JointCount())
    {
        return TrackStep::Waiting;
    }
    ASSISI_ASSERT(settings.underneath.size() == out.size(), "what lies underneath is a whole pose");

    bool rebound = false;
    if (settings.meshId != track.boundMeshId)
    {
        // A fade needs a pose to fade from that is this mesh's.
        EndFade(track);
        track.refillPending = true;
        track.bindingsStale = true;
        track.boundMeshId = settings.meshId;
    }
    if (track.bindingsStale)
    {
        BindClips(track.current, skeleton);
        BindClips(track.outgoing, skeleton);
        track.bindingsStale = false;
        track.warned = false;
        rebound = true;
    }

    if (track.fading)
    {
        if (track.snapshotPending)
        {
            track.fromPose.assign(out.begin(), out.end());
            track.toPose.assign(settings.underneath.begin(), settings.underneath.end());
            track.snapshotPending = false;
        }
        if (!track.outgoing.Sources.empty())
        {
            Geometry::SampleBlend(track.outgoing, skeleton, track.fromPose);
        }
        std::ranges::copy(settings.underneath, track.toPose.begin());
        Geometry::SampleBlend(track.current, skeleton, track.toPose);
        Geometry::MixPoses(track.fromPose, track.toPose, track.crossFade.Weight(), out);
    }
    else
    {
        if (settings.refill || track.refillPending)
        {
            std::ranges::copy(settings.underneath, out.begin());
        }
        Geometry::SampleBlend(track.current, skeleton, out);
    }
    track.refillPending = false;
    return rebound ? TrackStep::Rebound : TrackStep::Played;
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
void SlideWeight(const LayerRequest &layer, LayerState &state, float dt)
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
void RefreshMask(const LayerRequest &layer, LayerState &state, const Geometry::Skeleton &skeleton, uint32_t meshId)
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

bool Belongs(const LayerState &state, const LayerRequest &layer)
{
    return state.layerAnimation == layer.play.animation && state.layerMode == layer.mode;
}

bool Aligned(const AnimationPlayback &playback)
{
    const std::vector<LayerRequest> &layers = playback.request.layers;
    if (playback.layerStates.size() != layers.size())
    {
        return false;
    }
    for (std::size_t layer = 0; layer < layers.size(); ++layer)
    {
        if (!Belongs(playback.layerStates[layer], layers[layer]))
        {
            return false;
        }
    }
    return true;
}

/// Moves into each place @p placed has not filled the first state of @p old
/// not yet @p taken that @p fits it.
template <typename Fits>
void Place(AnimationPlayback &playback, std::vector<LayerState> &old, std::vector<uint8_t> &taken,
           std::vector<uint8_t> &placed, Fits fits)
{
    for (std::size_t layer = 0; layer < playback.request.layers.size(); ++layer)
    {
        for (std::size_t candidate = 0; candidate < old.size() && placed[layer] == 0; ++candidate)
        {
            if (taken[candidate] == 0 && fits(layer, candidate))
            {
                playback.layerStates[layer] = std::move(old[candidate]);
                // A state that moved took its count from another layer's
                // request; it starts level with this one's, so moving restarts nothing.
                if (candidate != layer)
                {
                    playback.layerStates[layer].track.restarts = playback.request.layers[layer].play.restarts;
                }
                taken[candidate] = 1;
                placed[layer] = 1;
            }
        }
    }
}

/// Puts back, as the base left them last frame, the joints any layer's mask
/// took then. The states are last frame's, read before the layers run again.
void RestoreUnderLayers(const AnimationPlayback &playback, Pose pose)
{
    if (playback.underLayers.size() != pose.size())
    {
        return;
    }
    for (const LayerState &state : playback.layerStates)
    {
        if (state.mask.size() != pose.size())
        {
            continue;
        }
        for (std::size_t joint = 0; joint < pose.size(); ++joint)
        {
            if (state.mask[joint] != 0)
            {
                pose[joint] = playback.underLayers[joint];
            }
        }
    }
}

/// What moving @p play on by @p dt needs: no pose, no mesh.
TrackSettings AdvanceSettings(const PlaybackRequest &play, float dt)
{
    return TrackSettings{.underneath = {},
                         .animation = play.animation,
                         .parameter = play.parameter,
                         .speed = play.speed,
                         .startPhase = play.startPhase,
                         .fade = play.fade,
                         .dt = dt,
                         .meshId = kUnboundMesh,
                         .restarts = play.restarts,
                         .loop = play.loop,
                         .refill = false};
}

/// What sampling into a pose of mesh @p meshId needs, over @p underneath.
TrackSettings SampleSettings(std::span<const Geometry::JointTransform> underneath, uint32_t meshId, bool refill)
{
    TrackSettings settings;
    settings.underneath = underneath;
    settings.meshId = meshId;
    settings.refill = refill;
    return settings;
}

bool Bound(const Geometry::Skeleton &skeleton, const SkinnedPose &skinned)
{
    return skinned.boundMeshId != kUnboundMesh && skinned.pose.size() == skeleton.JointCount();
}

} // namespace

bool AdvanceTrack(AnimationTrack &track, const TrackSettings &settings, const ResolvedAnimation &animation)
{
    return AdvanceClips(track, settings, animation) == TrackStep::Rebound;
}

bool SampleTrack(AnimationTrack &track, const TrackSettings &settings, const Geometry::Skeleton &skeleton,
                 std::span<Geometry::JointTransform> out)
{
    return SampleClips(track, settings, skeleton, out) == TrackStep::Rebound;
}

void RequestFromAuthored(const AnimationPlayer &player, PlayerRequest &request)
{
    PlaybackRequest &base = request.base;
    base.animation = player.animation;
    base.parameter = player.parameter;
    base.speed = player.speed;
    base.startPhase = 0.f;
    base.fade = player.fade;
    base.fadeKind = FadeKind::CrossFade;
    base.loop = player.loop;

    request.layers.resize(player.layers.size());
    for (std::size_t index = 0; index < player.layers.size(); ++index)
    {
        const AnimationLayer &authored = player.layers[index];
        LayerRequest &layer = request.layers[index];
        layer.play.animation = authored.animation;
        layer.play.parameter = authored.parameter;
        layer.play.speed = authored.speed;
        layer.play.startPhase = 0.f;
        layer.play.fade = authored.fade;
        layer.play.fadeKind = FadeKind::CrossFade;
        layer.play.loop = authored.loop;
        layer.exclusions = authored.exclusions;
        layer.maskRoot = authored.maskRoot;
        layer.weight = authored.weight;
        layer.weightFade = authored.weightFade;
        layer.mode = authored.mode;
        layer.weightSlide = authored.weightSlide;
    }
}

void AdvancePlayerTime(AnimationPlayback &playback, const ResolvedAnimation &animation, float dt)
{
    (void)AdvanceClips(playback.track, AdvanceSettings(playback.request.base, dt), animation);
}

bool SamplePlayerPose(AnimationPlayback &playback, const Geometry::Skeleton &skeleton, SkinnedPose &skinned)
{
    if (!Bound(skeleton, skinned))
    {
        return false;
    }
    RestoreUnderLayers(playback, skinned.pose);
    const TrackSettings settings = SampleSettings(skeleton.RestLocal, skinned.boundMeshId, /*refill=*/ false);
    const bool rebound = SampleClips(playback.track, settings, skeleton, skinned.pose) == TrackStep::Rebound;
    if (!playback.request.layers.empty())
    {
        playback.underLayers.assign(skinned.pose.begin(), skinned.pose.end());
    }
    return rebound;
}

bool AdvanceAnimationPlayer(AnimationPlayback &playback, const ResolvedAnimation &animation,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedPose &skinned)
{
    if (!Bound(skeleton, skinned))
    {
        return false;
    }
    AdvancePlayerTime(playback, animation, dt);
    return SamplePlayerPose(playback, skeleton, skinned);
}

void MatchLayerStates(AnimationPlayback &playback)
{
    if (Aligned(playback))
    {
        return;
    }
    const std::vector<LayerRequest> &layers = playback.request.layers;
    std::vector<LayerState> old = std::move(playback.layerStates);
    playback.layerStates.clear();
    playback.layerStates.resize(layers.size());
    std::vector<uint8_t> taken(old.size(), 0);
    std::vector<uint8_t> placed(layers.size(), 0);
    // Where it was, then where its layer moved to, then, for a layer that
    // changed, the state at its own place.
    Place(playback, old, taken, placed, [&](std::size_t layer, std::size_t candidate) {
            return layer == candidate && Belongs(old[candidate], layers[layer]);
        });
    Place(playback, old, taken, placed, [&](std::size_t layer, std::size_t candidate) {
            return Belongs(old[candidate], layers[layer]);
        });
    Place(playback, old, taken, placed, [](std::size_t layer, std::size_t candidate) { return layer == candidate; });
    for (std::size_t layer = 0; layer < layers.size(); ++layer)
    {
        playback.layerStates[layer].layerAnimation = layers[layer].play.animation;
        playback.layerStates[layer].layerMode = layers[layer].mode;
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

void AdvanceLayerTime(const LayerRequest &layer, LayerState &state, const ResolvedAnimation &animation, float dt)
{
    SlideWeight(layer, state, dt);
    const TrackStep step = AdvanceClips(state.track, AdvanceSettings(layer.play, dt), animation);
    // Kept until the layer is next sampled, which may be frames later at weight 0.
    if (step == TrackStep::Rebound && state.track.fading)
    {
        state.fadeStarted = true;
    }
}

bool SampleLayerPose(const LayerRequest &layer, LayerState &state, const Geometry::Skeleton &skeleton,
                     SkinnedPose &skinned)
{
    if (!Bound(skeleton, skinned))
    {
        return false;
    }
    RefreshMask(layer, state, skeleton, skinned.boundMeshId);
    // A layer at weight 0 shows nothing, so it is not sampled at all; its
    // timeline still moved on in AdvanceLayerTime.
    if (state.weight <= 0.f)
    {
        return false;
    }

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
    const TrackSettings settings = SampleSettings(underneath, skinned.boundMeshId, /*refill=*/ true);
    const TrackStep step = SampleClips(state.track, settings, skeleton, state.pose);
    if (step == TrackStep::Waiting)
    {
        return false;
    }
    if (additive)
    {
        UpdateReference(state, skeleton, state.fadeStarted);
        Add(state, skinned.pose);
    }
    else
    {
        Override(state, skinned.pose);
    }
    state.fadeStarted = false;
    return step == TrackStep::Rebound;
}

void AdvanceAnimationLayer(const LayerRequest &layer, LayerState &state, const ResolvedAnimation &animation,
                           const Geometry::Skeleton &skeleton, float dt, SkinnedPose &skinned)
{
    if (!Bound(skeleton, skinned))
    {
        return;
    }
    AdvanceLayerTime(layer, state, animation, dt);
    SampleLayerPose(layer, state, skeleton, skinned);
}

} // namespace Assisi::Runtime
