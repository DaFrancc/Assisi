/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AnimationPlayback.hpp>

#include <Assisi/Geometry/AnimationSampling.hpp>

#include <algorithm>
#include <utility>

namespace Assisi::Runtime
{

bool AdvanceAnimationPlayer(AnimationPlayer &player, std::shared_ptr<const Geometry::AnimationClip> clip,
                            const Geometry::Skeleton &skeleton, float dt, SkinnedMesh &skinned)
{
    if (clip == nullptr || skinned.boundMeshId == kUnboundMesh || skinned.pose.size() != skeleton.JointCount())
    {
        return false;
    }

    const bool rebind = clip != player.loaded || player.clip != player.boundClip ||
                        skinned.boundMeshId != player.boundMeshId;
    if (rebind)
    {
        // A clip only moves the joints it has tracks for. Without this, joints
        // a clip played before this one moved would stay where it left them.
        std::ranges::copy(skeleton.RestLocal, skinned.pose.begin());
        player.binding = Geometry::BindClip(*clip, skeleton);
        if (player.clip != player.boundClip)
        {
            player.time = 0.f;
        }
        player.loaded = std::move(clip);
        player.boundClip = player.clip;
        player.boundMeshId = skinned.boundMeshId;
        player.warned = false;
    }

    const Geometry::AnimationClip &playing = *player.loaded;
    player.time = Geometry::WrapClipTime(player.time + dt * player.speed, playing.Duration, player.loop);
    Geometry::SampleClip(playing, player.binding, player.time, skinned.pose);
    return rebind;
}

} // namespace Assisi::Runtime
