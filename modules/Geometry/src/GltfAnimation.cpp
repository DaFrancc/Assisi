/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "GltfAnimation.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace Assisi::Geometry
{

namespace
{

/// The node each node is a child of, or the node count for a root.
std::vector<std::size_t> ParentsOf(const fastgltf::Asset &asset)
{
    std::vector<std::size_t> parents(asset.nodes.size(), asset.nodes.size());
    for (std::size_t node = 0; node < asset.nodes.size(); ++node)
    {
        for (const std::size_t child : asset.nodes[node].children)
        {
            if (child < parents.size())
            {
                parents[child] = node;
            }
        }
    }
    return parents;
}

bool StrictlyIncreasing(std::span<const float> times)
{
    for (std::size_t key = 0; key < times.size(); ++key)
    {
        if (!std::isfinite(times[key]) || (key > 0 && times[key] <= times[key - 1]))
        {
            return false;
        }
    }
    return true;
}

bool Finite(const glm::vec3 &value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const glm::quat &value)
{
    return std::isfinite(value.w) && std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

void ReadValues(const fastgltf::Asset &asset, const fastgltf::Accessor &accessor, std::vector<glm::vec3> &out)
{
    out.reserve(accessor.count);
    fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, accessor, [&out](fastgltf::math::fvec3 value)
                                                     { out.emplace_back(value.x(), value.y(), value.z()); });
}

/// glTF keeps a quaternion as x y z w. Renormalised, since a rotation stored as
/// normalised integers comes back a hair off unit length.
void ReadValues(const fastgltf::Asset &asset, const fastgltf::Accessor &accessor, std::vector<glm::quat> &out)
{
    out.reserve(accessor.count);
    fastgltf::iterateAccessor<fastgltf::math::fvec4>(
        asset, accessor, [&out](fastgltf::math::fvec4 value)
        { out.push_back(glm::normalize(glm::quat(value.w(), value.x(), value.y(), value.z()))); });
}

/// Reads one sampler's keys into @p channel. Its interpolation was checked by
/// the caller.
template <typename T>
std::expected<void, AnimationReadError> ReadChannel(const fastgltf::Asset &asset,
                                                    const fastgltf::AnimationSampler &sampler, Channel<T> &channel)
{
    if (sampler.inputAccessor >= asset.accessors.size() || sampler.outputAccessor >= asset.accessors.size())
    {
        return std::unexpected(AnimationReadError::BadKeys);
    }
    if (!channel.Empty())
    {
        // Two channels on one property of one node: which wins is not defined.
        return std::unexpected(AnimationReadError::BadKeys);
    }
    const fastgltf::Accessor &input = asset.accessors[sampler.inputAccessor];
    const fastgltf::Accessor &output = asset.accessors[sampler.outputAccessor];
    channel.Times.reserve(input.count);
    fastgltf::iterateAccessor<float>(asset, input, [&channel](float time) { channel.Times.push_back(time); });
    ReadValues(asset, output, channel.Values);
    channel.Mode = sampler.interpolation == fastgltf::AnimationInterpolation::Step ? Interpolation::Step
                                                                                   : Interpolation::Linear;

    const bool finite = std::ranges::all_of(channel.Values, [](const T &value) { return Finite(value); });
    if (channel.Times.size() != channel.Values.size() || !StrictlyIncreasing(channel.Times) || !finite)
    {
        return std::unexpected(AnimationReadError::BadKeys);
    }
    return {};
}

/// The track for @p node, made when the animation first moves it.
std::expected<std::size_t, AnimationReadError> TrackFor(const fastgltf::Asset &asset, std::size_t node,
                                                        GltfClip &read,
                                                        std::unordered_map<std::size_t, std::size_t> &trackOfNode)
{
    const std::unordered_map<std::size_t, std::size_t>::const_iterator found = trackOfNode.find(node);
    if (found != trackOfNode.end())
    {
        return found->second;
    }
    const std::string_view name{asset.nodes[node].name};
    if (name.empty())
    {
        return std::unexpected(AnimationReadError::UnnamedJoint);
    }
    for (const JointTrack &track : read.clip.Tracks)
    {
        if (track.Joint == name)
        {
            return std::unexpected(AnimationReadError::DuplicateJoint);
        }
    }
    const std::size_t track = read.clip.Tracks.size();
    JointTrack added;
    added.Joint = std::string{name};
    read.clip.Tracks.push_back(std::move(added));
    read.nodeOfTrack.push_back(node);
    trackOfNode.emplace(node, track);
    return track;
}

template <typename T> float LastKey(const Channel<T> &channel)
{
    return channel.Empty() ? 0.f : channel.Times.back();
}

// ---- Writing ----------------------------------------------------------------

/// The parts of a glTF being written, and the one buffer under them.
struct GlbBuilder
{
    fastgltf::Asset asset;
    std::vector<std::byte> bytes;

    /// Appends @p floats as one accessor of @p type, @p count elements long.
    std::size_t AddAccessor(std::span<const float> floats, fastgltf::AccessorType type, std::size_t count)
    {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + floats.size_bytes());
        std::memcpy(bytes.data() + offset, floats.data(), floats.size_bytes());

        fastgltf::BufferView view;
        view.bufferIndex = 0;
        view.byteOffset = offset;
        view.byteLength = floats.size_bytes();
        asset.bufferViews.push_back(std::move(view));

        fastgltf::Accessor accessor;
        accessor.count = count;
        accessor.type = type;
        accessor.componentType = fastgltf::ComponentType::Float;
        accessor.bufferViewIndex = asset.bufferViews.size() - 1;
        asset.accessors.push_back(std::move(accessor));
        return asset.accessors.size() - 1;
    }

    /// Key times, with the bounds glTF requires of an animation's inputs.
    std::size_t AddTimes(std::span<const float> times)
    {
        const std::size_t index = AddAccessor(times, fastgltf::AccessorType::Scalar, times.size());
        for (const float time : times)
        {
            asset.accessors[index].updateBoundsToInclude(static_cast<double>(time));
        }
        return index;
    }
};

std::vector<float> Flatten(std::span<const glm::vec3> values)
{
    std::vector<float> floats;
    floats.reserve(values.size() * 3);
    for (const glm::vec3 &value : values)
    {
        floats.insert(floats.end(), {value.x, value.y, value.z});
    }
    return floats;
}

/// Back to glTF's x y z w.
std::vector<float> Flatten(std::span<const glm::quat> values)
{
    std::vector<float> floats;
    floats.reserve(values.size() * 4);
    for (const glm::quat &value : values)
    {
        floats.insert(floats.end(), {value.x, value.y, value.z, value.w});
    }
    return floats;
}

template <typename T>
void AddChannel(GlbBuilder &builder, const Channel<T> &channel, std::size_t node, fastgltf::AnimationPath path)
{
    if (channel.Empty())
    {
        return;
    }
    constexpr fastgltf::AccessorType kType =
        std::is_same_v<T, glm::quat> ? fastgltf::AccessorType::Vec4 : fastgltf::AccessorType::Vec3;
    const std::size_t input = builder.AddTimes(channel.Times);
    const std::vector<float> values = Flatten(std::span<const T>{channel.Values});
    const std::size_t output = builder.AddAccessor(values, kType, channel.Values.size());

    fastgltf::Animation &animation = builder.asset.animations.front();
    animation.samplers.push_back(fastgltf::AnimationSampler{
                .inputAccessor = input,
                .outputAccessor = output,
                .interpolation = channel.Mode == Interpolation::Step ? fastgltf::AnimationInterpolation::Step
                                                                 : fastgltf::AnimationInterpolation::Linear});
    animation.channels.push_back(
        fastgltf::AnimationChannel{.samplerIndex = animation.samplers.size() - 1, .nodeIndex = node, .path = path});
}

/// The nodes a clip file keeps from @p source: every node a track came from, and
/// every node above one, in the source's order.
std::vector<bool> KeptNodes(const GltfClip &clip, const fastgltf::Asset &source)
{
    const std::vector<std::size_t> parents = ParentsOf(source);
    std::vector<bool> kept(source.nodes.size(), false);
    for (std::size_t node : clip.nodeOfTrack)
    {
        while (node < source.nodes.size() && !kept[node])
        {
            kept[node] = true;
            node = parents[node];
        }
    }
    return kept;
}

/// Copies the kept nodes of @p source into @p builder, renumbered, with their
/// names, rest transforms and kept children, and a scene of the kept roots.
/// Returns each source node's new index.
std::vector<std::size_t> CopyNodes(GlbBuilder &builder, const fastgltf::Asset &source, const std::vector<bool> &kept)
{
    const std::size_t dropped = source.nodes.size();
    std::vector<std::size_t> renumbered(source.nodes.size(), dropped);
    for (std::size_t node = 0; node < source.nodes.size(); ++node)
    {
        if (kept[node])
        {
            renumbered[node] = builder.asset.nodes.size();
            fastgltf::Node copy;
            copy.name = source.nodes[node].name;
            copy.transform = source.nodes[node].transform;
            builder.asset.nodes.push_back(std::move(copy));
        }
    }

    const std::vector<std::size_t> parents = ParentsOf(source);
    fastgltf::Scene scene;
    for (std::size_t node = 0; node < source.nodes.size(); ++node)
    {
        if (!kept[node])
        {
            continue;
        }
        for (const std::size_t child : source.nodes[node].children)
        {
            if (child < kept.size() && kept[child])
            {
                builder.asset.nodes[renumbered[node]].children.push_back(renumbered[child]);
            }
        }
        if (parents[node] == dropped || !kept[parents[node]])
        {
            scene.nodeIndices.push_back(renumbered[node]);
        }
    }
    builder.asset.scenes.push_back(std::move(scene));
    builder.asset.defaultScene = 0;
    return renumbered;
}

} // namespace

std::expected<GltfClip, AnimationReadError> ReadGltfAnimation(const fastgltf::Asset &asset, std::size_t index)
{
    const fastgltf::Animation &animation = asset.animations[index];
    GltfClip read;
    read.clip.Name = std::string{animation.name};
    std::unordered_map<std::size_t, std::size_t> trackOfNode;

    for (const fastgltf::AnimationChannel &channel : animation.channels)
    {
        if (!channel.nodeIndex.has_value() || *channel.nodeIndex >= asset.nodes.size() ||
            channel.samplerIndex >= animation.samplers.size())
        {
            return std::unexpected(AnimationReadError::BadKeys);
        }
        const fastgltf::AnimationSampler &sampler = animation.samplers[channel.samplerIndex];
        if (channel.path == fastgltf::AnimationPath::Weights)
        {
            return std::unexpected(AnimationReadError::MorphWeights);
        }
        if (sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline)
        {
            return std::unexpected(AnimationReadError::CubicSpline);
        }

        const std::expected<std::size_t, AnimationReadError> track =
            TrackFor(asset, *channel.nodeIndex, read, trackOfNode);
        if (!track)
        {
            return std::unexpected(track.error());
        }
        JointTrack &keys = read.clip.Tracks[*track];
        std::expected<void, AnimationReadError> channelRead;
        switch (channel.path)
        {
        case fastgltf::AnimationPath::Rotation:
            channelRead = ReadChannel(asset, sampler, keys.Rotation);
            break;
        case fastgltf::AnimationPath::Translation:
            channelRead = ReadChannel(asset, sampler, keys.Translation);
            break;
        default:
            channelRead = ReadChannel(asset, sampler, keys.Scale);
            break;
        }
        if (!channelRead)
        {
            return std::unexpected(channelRead.error());
        }
    }

    for (const JointTrack &track : read.clip.Tracks)
    {
        read.clip.Duration = std::max({read.clip.Duration, LastKey(track.Rotation), LastKey(track.Translation),
                                       LastKey(track.Scale)});
    }
    if (read.clip.Tracks.empty())
    {
        return std::unexpected(AnimationReadError::NoKeys);
    }
    return read;
}

std::vector<std::byte> WriteClipGlb(const GltfClip &clip, const fastgltf::Asset &source)
{
    GlbBuilder builder;
    builder.asset.assetInfo = fastgltf::AssetInfo{.gltfVersion = "2.0", .copyright = {}, .generator = "Assisi"};
    const std::vector<std::size_t> renumbered = CopyNodes(builder, source, KeptNodes(clip, source));

    fastgltf::Animation animation;
    animation.name = clip.clip.Name;
    builder.asset.animations.push_back(std::move(animation));
    for (std::size_t track = 0; track < clip.clip.Tracks.size(); ++track)
    {
        const JointTrack &keys = clip.clip.Tracks[track];
        const std::size_t node = renumbered[clip.nodeOfTrack[track]];
        AddChannel(builder, keys.Rotation, node, fastgltf::AnimationPath::Rotation);
        AddChannel(builder, keys.Translation, node, fastgltf::AnimationPath::Translation);
        AddChannel(builder, keys.Scale, node, fastgltf::AnimationPath::Scale);
    }

    fastgltf::Buffer buffer;
    buffer.byteLength = builder.bytes.size();
    buffer.data = fastgltf::sources::Vector{.bytes = std::move(builder.bytes)};
    builder.asset.buffers.push_back(std::move(buffer));

    fastgltf::Exporter exporter;
    fastgltf::Expected<fastgltf::ExportResult<std::vector<std::byte>>> written =
        exporter.writeGltfBinary(builder.asset);
    if (written.error() != fastgltf::Error::None)
    {
        return {};
    }
    return std::move(written.get().output);
}

} // namespace Assisi::Geometry
