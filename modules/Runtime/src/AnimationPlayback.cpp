/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AnimationPlayback.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Geometry/AnimationBlend.hpp>
#include <Assisi/Geometry/AnimationSampling.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Assisi::Runtime
{

namespace
{

using ClipList = std::span<const std::shared_ptr<const Geometry::AnimationClip>>;

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

bool NeedsRebind(const AnimationPlayer &player, const ResolvedAnimation &animation, const SkinnedMesh &skinned)
{
    return player.animation != player.boundAnimation || skinned.boundMeshId != player.boundMeshId ||
           animation.space != player.space || !SameClips(player.current, animation.clips);
}

/// Whatever was fading out stops, so only the current animation plays.
void EndFade(AnimationPlayer &player)
{
    player.fading = false;
    player.outgoing.Sources.clear();
    player.outgoing.Weights.clear();
}

/// Fades from the pose as it is. A fade already running is frozen where it
/// got to, so the new fade starts from what was on screen.
void StartFade(AnimationPlayer &player, const Geometry::Skeleton &skeleton, std::span<const Geometry::JointTransform> pose)
{
    player.fromPose.assign(pose.begin(), pose.end());
    if (player.fading)
    {
        player.outgoing.Sources.clear();
        player.outgoing.Weights.clear();
    }
    else
    {
        // Swapped rather than copied, so the next animation reuses the buffers.
        std::swap(player.outgoing, player.current);
    }
    // The new animation writes only the joints it moves; the rest fade to rest.
    player.toPose = skeleton.RestLocal;
    player.crossFade = Geometry::CrossFade{.Elapsed = 0.f, .Duration = player.fade};
    player.fading = true;
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

void TakePositions(AnimationPlayer &player, const Geometry::BlendSpace *space)
{
    player.positions.clear();
    if (space == nullptr)
    {
        return;
    }
    for (const Geometry::BlendPoint &point : space->Points)
    {
        player.positions.push_back(point.Position);
    }
}

void Rebind(AnimationPlayer &player, const ResolvedAnimation &animation, const Geometry::Skeleton &skeleton,
            SkinnedMesh &skinned)
{
    const bool changed = player.animation != player.boundAnimation;
    // Fading needs a pose to fade from that is this mesh's.
    const bool fades = changed && player.fade > 0.f && skinned.boundMeshId == player.boundMeshId;
    if (fades)
    {
        StartFade(player, skeleton, skinned.pose);
    }
    else
    {
        EndFade(player);
        // An animation only moves the joints it has tracks for. Without this,
        // joints one played before this one moved would stay where it left them.
        std::ranges::copy(skeleton.RestLocal, skinned.pose.begin());
    }

    BindClips(player.current, animation.clips, skeleton);
    if (changed)
    {
        player.current.Phase = 0.f;
    }
    player.space = animation.space;
    TakePositions(player, animation.space.get());
    player.boundAnimation = player.animation;
    player.boundMeshId = skinned.boundMeshId;
    player.warned = false;
}

void Fade(AnimationPlayer &player, const Geometry::Skeleton &skeleton, float step, float dt, SkinnedMesh &skinned)
{
    if (!player.outgoing.Sources.empty())
    {
        Geometry::AdvancePhase(player.outgoing, step, player.loop);
        Geometry::SampleBlend(player.outgoing, skeleton, player.fromPose);
    }
    Geometry::SampleBlend(player.current, skeleton, player.toPose);
    player.crossFade.Advance(dt);
    Geometry::MixPoses(player.fromPose, player.toPose, player.crossFade.Weight(), skinned.pose);
    if (player.crossFade.Done())
    {
        std::ranges::copy(player.toPose, skinned.pose.begin());
        EndFade(player);
    }
}

} // namespace

bool AdvanceAnimationPlayer(AnimationPlayer &player, const ResolvedAnimation &animation,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned)
{
    // No animation is no clips; an animation with none has not loaded yet.
    const bool loaded = player.animation.IsNil() || !animation.clips.empty();
    if (!loaded || std::ranges::contains(animation.clips, nullptr) || skinned.boundMeshId == kUnboundMesh ||
        skinned.pose.size() != skeleton.JointCount())
    {
        return false;
    }
    ASSISI_ASSERT(animation.space == nullptr || animation.space->Points.size() == animation.clips.size(),
                  "a clip per point");

    const bool rebind = NeedsRebind(player, animation, skinned);
    if (rebind)
    {
        Rebind(player, animation, skeleton, skinned);
    }

    if (player.space != nullptr)
    {
        Geometry::BlendWeights(player.positions, player.parameter, player.current.Weights);
    }
    // The timeline follows `speed`; a fade's seconds do not.
    const float step = dt * player.speed;
    Geometry::AdvancePhase(player.current, step, player.loop);
    if (player.fading)
    {
        Fade(player, skeleton, step, dt, skinned);
    }
    else
    {
        Geometry::SampleBlend(player.current, skeleton, skinned.pose);
    }
    return rebind;
}

} // namespace Assisi::Runtime
