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
#include <Assisi/Runtime/AnimatorGraph.hpp>
#include <Assisi/Runtime/AnimatorStep.hpp>
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

void WarnMissingJoints(Runtime::AnimationTrack &track)
{
    if (track.warned)
    {
        return;
    }
    for (const Geometry::BlendSource &source : track.current.Sources)
    {
        const std::string missing = MissingJoints(*source.Clip, source.Binding);
        if (!missing.empty())
        {
            Core::Log::Warn("Animation: clip '{}' moves joints the mesh does not have, which it skips: {}.",
                            source.Clip->Name, missing);
        }
    }
    track.warned = true;
}

void WarnMissingMaskJoints(const Runtime::AnimationLayer &layer, Runtime::LayerState &state, std::size_t index)
{
    if (state.maskWarned || (!state.rootMissing && !state.exclusionMissing))
    {
        return;
    }
    if (state.rootMissing)
    {
        Core::Log::Warn("Animation: layer {}'s mask starts at joint '{}', which the mesh does not have, so it moves "
                        "nothing.",
                        index, layer.maskRoot.View());
    }
    if (state.exclusionMissing)
    {
        Core::Log::Warn("Animation: layer {}'s mask leaves out joints the mesh does not have, which it skips.", index);
    }
    state.maskWarned = true;
}

/// Fills track.resolved with @p space's clips, one per point; false while any
/// has not loaded.
bool ResolveSpaceClips(Core::AssetStore &assets, const Geometry::BlendSpace &space, Runtime::AnimationTrack &track)
{
    track.resolved.resize(space.Points.size());
    bool loaded = true;
    for (std::size_t point = 0; point < space.Points.size(); ++point)
    {
        // Every point asks, so each clip's load starts on the first frame.
        track.resolved[point] = assets.Resolve<Geometry::AnimationClip>(space.Points[point].Clip);
        loaded = loaded && track.resolved[point] != nullptr;
    }
    return loaded;
}

/// What @p animation resolves to this frame, its clips held in
/// track.resolved; nothing while it or any of its clips is loading.
std::optional<Runtime::ResolvedAnimation> ResolveAnimation(Core::AssetStore &assets, Core::AssetId animation,
                                                           Runtime::AnimationTrack &track)
{
    if (animation.IsNil())
    {
        track.resolved.clear();
        return Runtime::ResolvedAnimation{};
    }
    const ClipOrSpace found = assets.ResolveOneOf<Geometry::AnimationClip, Geometry::BlendSpace>(animation);
    if (const std::shared_ptr<const Geometry::AnimationClip> *clip = std::get_if<1>(&found))
    {
        track.resolved.assign(1, *clip);
        return Runtime::ResolvedAnimation{.space = nullptr, .clips = track.resolved};
    }
    if (const std::shared_ptr<const Geometry::BlendSpace> *space = std::get_if<2>(&found))
    {
        if (ResolveSpaceClips(assets, **space, track))
        {
            return Runtime::ResolvedAnimation{.space = *space, .clips = track.resolved};
        }
    }
    return std::nullopt;
}

/// Plays each of @p player's layers over the pose its base wrote, in order.
void AdvanceLayers(Core::AssetStore &assets, Runtime::AnimationPlayer &player, const Geometry::Skeleton &skeleton,
                   float dt, Runtime::SkinnedMesh &skinned)
{
    Runtime::MatchLayerStates(player);
    for (std::size_t index = 0; index < player.layers.size(); ++index)
    {
        const Runtime::AnimationLayer &layer = player.layers[index];
        Runtime::LayerState &state = player.layerStates[index];
        const std::optional<Runtime::ResolvedAnimation> animation =
            ResolveAnimation(assets, layer.animation, state.track);
        if (!animation.has_value())
        {
            continue;
        }
        Runtime::AdvanceAnimationLayer(layer, state, *animation, skeleton, dt, skinned);
        WarnMissingJoints(state.track);
        WarnMissingMaskJoints(layer, state, index);
    }
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
        if (Runtime::BindSkinnedMesh(skinned, renderer) == nullptr)
        {
            continue;
        }
        const Geometry::Skeleton &skeleton = renderer.meshBuffer->Skeleton();
        const std::optional<Runtime::ResolvedAnimation> animation =
            ResolveAnimation(*ctx.assets, player.animation, player.track);
        if (animation.has_value() && Runtime::AdvanceAnimationPlayer(player, *animation, skeleton, ctx.dt, skinned))
        {
            WarnMissingJoints(player.track);
        }
        AdvanceLayers(*ctx.assets, player, skeleton, ctx.dt, skinned);
    }
}

void AnimatorSystem(SystemContext &ctx)
{
    if (ctx.assets == nullptr)
    {
        return;
    }
    for (auto [entity, animator, player] :
         ctx.world.scene.Query<Mut<Runtime::Animator>, Mut<Runtime::AnimationPlayer>>())
    {
        std::shared_ptr<const Runtime::AnimatorGraph> graph =
            ctx.assets->Resolve<Runtime::AnimatorGraph>(animator.machine);
        // Null while a changed file cooks again, or when it won't: the one bound plays on.
        if (graph != nullptr && graph != animator.run.graph)
        {
            Runtime::BindAnimator(animator, player, std::move(graph));
        }
        Runtime::StepAnimator(animator, player);
    }
}

} // namespace Assisi::App
