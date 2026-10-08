/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/AnimationSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Runtime/AnimationPlayback.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace Assisi::App
{

namespace
{

/// The joints @p clip names that its binding found no joint for, comma-separated.
std::string MissingJoints(const Geometry::AnimationClip &clip, const Geometry::ClipBinding &binding)
{
    std::string missing;
    for (std::size_t track = 0; track < clip.Tracks.size(); ++track)
    {
        if (binding.JointOfTrack[track] != Geometry::kNoJoint)
        {
            continue;
        }
        if (!missing.empty())
        {
            missing += ", ";
        }
        missing += clip.Tracks[track].Joint;
    }
    return missing;
}

void WarnMissingJoints(Runtime::AnimationPlayer &player)
{
    const std::string missing = MissingJoints(*player.loaded, player.binding);
    if (missing.empty() || player.warned)
    {
        return;
    }
    Core::Log::Warn("Animation: clip '{}' moves joints the mesh does not have, which it skips: {}.",
                    player.loaded->Name, missing);
    player.warned = true;
}

} // namespace

void AnimationPlayerSystem(SystemContext &ctx)
{
    if (ctx.assets == nullptr)
    {
        return;
    }
    for (auto [entity, player, skinned, renderer] :
         ctx.world.scene.Query<Mut<Runtime::AnimationPlayer>, Mut<Runtime::SkinnedMesh>, Runtime::MeshRenderer>())
    {
        if (player.clip.IsNil() || Runtime::BindSkinnedMesh(skinned, renderer) == nullptr)
        {
            continue;
        }
        std::shared_ptr<const Geometry::AnimationClip> clip = ctx.assets->Resolve<Geometry::AnimationClip>(player.clip);
        if (Runtime::AdvanceAnimationPlayer(player, std::move(clip), renderer.meshBuffer->Skeleton(), ctx.dt, skinned))
        {
            WarnMissingJoints(player);
        }
    }
}

} // namespace Assisi::App
