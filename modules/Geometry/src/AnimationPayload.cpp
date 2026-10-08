/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "AnimationPayload.hpp"

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Geometry/AnimationFile.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>

namespace Assisi::Geometry
{

namespace
{

constexpr std::size_t kBitsPerByte = 8;
constexpr std::size_t kFloatBytes = sizeof(float);
constexpr std::size_t kVec3Floats = 3;
constexpr std::size_t kQuatFloats = 4;

/// The least a track takes in a payload: an empty joint name's length byte, and
/// three channels each of a mode byte and a key count of zero.
constexpr std::size_t kChannelsPerTrack = 3;
constexpr std::size_t kMinTrackBytes = 1 + kChannelsPerTrack * 2;

/// Longest joint or clip name a payload may carry.
constexpr std::size_t kMaxNameBytes = 256;

/// Reads a count, and refuses one larger than the bytes left could hold at
/// @p recordBytes each, so a corrupt count cannot ask for a huge allocation.
bool ReadCount(Core::BitReader &reader, std::size_t recordBytes, std::uint32_t &count)
{
    count = reader.ReadVarUInt32();
    if (reader.Failed())
    {
        return false;
    }
    const std::size_t bytesLeft = reader.BitsRemaining() / kBitsPerByte;
    return static_cast<std::size_t>(count) <= bytesLeft / recordBytes;
}

void WriteValue(Core::BitWriter &writer, const glm::vec3 &value)
{
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
}

void WriteValue(Core::BitWriter &writer, const glm::quat &value)
{
    writer.WriteFloat(value.w);
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
}

void ReadValue(Core::BitReader &reader, glm::vec3 &value)
{
    value.x = reader.ReadFloat();
    value.y = reader.ReadFloat();
    value.z = reader.ReadFloat();
}

void ReadValue(Core::BitReader &reader, glm::quat &value)
{
    value.w = reader.ReadFloat();
    value.x = reader.ReadFloat();
    value.y = reader.ReadFloat();
    value.z = reader.ReadFloat();
}

template <typename T> void WriteChannel(Core::BitWriter &writer, const Channel<T> &channel)
{
    ASSISI_ASSERT(channel.Times.size() == channel.Values.size(), "a value per key time");
    writer.WriteUInt8(static_cast<std::uint8_t>(channel.Mode));
    writer.WriteVarUInt32(static_cast<std::uint32_t>(channel.Times.size()));
    for (const float time : channel.Times)
    {
        writer.WriteFloat(time);
    }
    for (const T &value : channel.Values)
    {
        WriteValue(writer, value);
    }
}

template <typename T> bool ReadChannel(Core::BitReader &reader, std::size_t valueFloats, Channel<T> &channel)
{
    const std::uint8_t mode = reader.ReadUInt8();
    if (reader.Failed() || mode >= static_cast<std::uint8_t>(Interpolation::Count_))
    {
        return false;
    }
    channel.Mode = static_cast<Interpolation>(mode);

    std::uint32_t count = 0;
    if (!ReadCount(reader, (1 + valueFloats) * kFloatBytes, count))
    {
        return false;
    }
    channel.Times.resize(count);
    channel.Values.resize(count);
    for (float &time : channel.Times)
    {
        time = reader.ReadFloat();
    }
    for (T &value : channel.Values)
    {
        ReadValue(reader, value);
    }
    return !reader.Failed();
}

template <typename T> float LastKey(const Channel<T> &channel)
{
    return channel.Empty() ? 0.f : channel.Times.back();
}

} // namespace

std::string_view ToString(AnimationReadError error) noexcept
{
    switch (error)
    {
    case AnimationReadError::ParseFailed:
        return "the file is not a glTF that can be read";
    case AnimationReadError::NotOneClip:
        return "a clip file holds exactly one animation; use Extract animations on the model";
    case AnimationReadError::ExternalBuffer:
        return "a clip file keeps its keys inside itself; export it as .glb";
    case AnimationReadError::UnnamedJoint:
        return "the animation moves a node with no name, which no skeleton can match";
    case AnimationReadError::DuplicateJoint:
        return "the animation moves two nodes with the same name";
    case AnimationReadError::CubicSpline:
        return "the animation uses cubic-spline keys, which are not supported yet";
    case AnimationReadError::MorphWeights:
        return "the animation drives morph-target weights, which are not supported yet";
    case AnimationReadError::BadKeys:
        return "the animation's key times do not increase, or a key is not a number";
    case AnimationReadError::NoKeys:
        return "the animation has no keys";
    default:
        ASSISI_ASSERT(false, "ToString reached an AnimationReadError with no description");
        Core::Log::Error("AnimationFile: no description for this error");
        return "the animation cannot be read";
    }
}

Core::AssetError ToAssetError(AnimationReadError error) noexcept
{
    const bool unsupported = error == AnimationReadError::CubicSpline || error == AnimationReadError::MorphWeights ||
                             error == AnimationReadError::ExternalBuffer;
    return Core::AssetError{unsupported ? Core::AssetErrorCode::UnsupportedEncoding : Core::AssetErrorCode::CorruptAsset,
                            ToString(error)};
}

std::vector<std::byte> WriteAnimationPayload(const AnimationClip &clip)
{
    Core::BitWriter writer;
    writer.WriteVarUInt32(kAnimationPayloadVersion);
    writer.WriteString(clip.Name);
    writer.WriteVarUInt32(static_cast<std::uint32_t>(clip.Tracks.size()));
    for (const JointTrack &track : clip.Tracks)
    {
        writer.WriteString(track.Joint);
        WriteChannel(writer, track.Rotation);
        WriteChannel(writer, track.Translation);
        WriteChannel(writer, track.Scale);
    }
    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

std::expected<AnimationClip, Core::AssetError> LoadAnimation(std::span<const std::byte> payload)
{
    const Core::AssetError corrupt{Core::AssetErrorCode::CorruptAsset, "the cooked clip is cut short or damaged"};
    Core::BitReader reader{payload};
    const std::uint32_t version = reader.ReadVarUInt32();
    if (reader.Failed() || version != kAnimationPayloadVersion)
    {
        return std::unexpected(Core::AssetError{Core::AssetErrorCode::CorruptAsset,
                                                "the clip was cooked by another version; cook it again"});
    }

    AnimationClip clip;
    clip.Name = reader.ReadString(kMaxNameBytes);
    std::uint32_t trackCount = 0;
    if (reader.Failed() || !ReadCount(reader, kMinTrackBytes, trackCount))
    {
        return std::unexpected(corrupt);
    }
    clip.Tracks.resize(trackCount);
    for (JointTrack &track : clip.Tracks)
    {
        track.Joint = reader.ReadString(kMaxNameBytes);
        if (reader.Failed() || !ReadChannel(reader, kQuatFloats, track.Rotation) ||
            !ReadChannel(reader, kVec3Floats, track.Translation) || !ReadChannel(reader, kVec3Floats, track.Scale))
        {
            return std::unexpected(corrupt);
        }
        clip.Duration = std::max({clip.Duration, LastKey(track.Rotation), LastKey(track.Translation),
                                  LastKey(track.Scale)});
    }
    return clip;
}

} // namespace Assisi::Geometry
