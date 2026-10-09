/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/AnimationSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/AnimationBlend.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Runtime/AnimationPlayback.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/SkinnedMeshPose.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace Assisi::App
{

namespace
{

using ClipOrSpace = std::variant<std::monostate, std::shared_ptr<const Geometry::AnimationClip>,
                                 std::shared_ptr<const Geometry::BlendSpace>>;

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
    if (player.warned)
    {
        return;
    }
    for (const Geometry::BlendSource &source : player.current.Sources)
    {
        const std::string missing = MissingJoints(*source.Clip, source.Binding);
        if (!missing.empty())
        {
            Core::Log::Warn("Animation: clip '{}' moves joints the mesh does not have, which it skips: {}.",
                            source.Clip->Name, missing);
        }
    }
    player.warned = true;
}

/// Fills player.resolved with @p space's clips, one per point; false while any
/// has not loaded.
bool ResolveSpaceClips(Core::AssetStore &assets, const Geometry::BlendSpace &space, Runtime::AnimationPlayer &player)
{
    player.resolved.resize(space.Points.size());
    bool loaded = true;
    for (std::size_t point = 0; point < space.Points.size(); ++point)
    {
        // Every point asks, so each clip's load starts on the first frame.
        player.resolved[point] = assets.Resolve<Geometry::AnimationClip>(space.Points[point].Clip);
        loaded = loaded && player.resolved[point] != nullptr;
    }
    return loaded;
}

/// What @p player's animation resolves to this frame, its clips held in
/// player.resolved; nothing while it or any of its clips is loading.
std::optional<Runtime::ResolvedAnimation> ResolveAnimation(Core::AssetStore &assets, Runtime::AnimationPlayer &player)
{
    const ClipOrSpace found = assets.ResolveOneOf<Geometry::AnimationClip, Geometry::BlendSpace>(player.animation);
    if (const std::shared_ptr<const Geometry::AnimationClip> *clip = std::get_if<1>(&found))
    {
        player.resolved.assign(1, *clip);
        return Runtime::ResolvedAnimation{.space = nullptr, .clips = player.resolved};
    }
    if (const std::shared_ptr<const Geometry::BlendSpace> *space = std::get_if<2>(&found))
    {
        if (ResolveSpaceClips(assets, **space, player))
        {
            return Runtime::ResolvedAnimation{.space = *space, .clips = player.resolved};
        }
    }
    return std::nullopt;
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
        if (player.animation.IsNil() || Runtime::BindSkinnedMesh(skinned, renderer) == nullptr)
        {
            continue;
        }
        const std::optional<Runtime::ResolvedAnimation> animation = ResolveAnimation(*ctx.assets, player);
        if (animation.has_value() &&
            Runtime::AdvanceAnimationPlayer(player, *animation, renderer.meshBuffer->Skeleton(), ctx.dt, skinned))
        {
            WarnMissingJoints(player);
        }
    }
}

} // namespace Assisi::App
