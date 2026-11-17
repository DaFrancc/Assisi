/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestRig.hpp
/// @brief Characters built in code for tests: a skeleton of any shape, a box
/// mesh skinned to it, and clips whose numbers are known exactly.
///
/// The real test characters are git-ignored, and they carry one asset author's
/// conventions. These cover what they can't: any number of limbs of two or
/// three segments, bones along another axis, and clips a test can check to the
/// number. Nothing here assumes a human: limbs are limbs, named `<limb>_0`,
/// `<limb>_1`, ... `<limb>_end` from the body outwards.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/Bounds.hpp>
#include <Assisi/Geometry/MeshData.hpp>
#include <Assisi/Geometry/Pose.hpp>
#include <Assisi/Math/GLM.hpp>

namespace Assisi::Runtime::Testing
{

/// @brief Which way a limb joint's bone points, in its own frame, toward its
///        child.
enum class BoneAxis : uint8_t
{
    PlusY, ///< As glTF and Blender exports have it.
    PlusX, ///< As Maya and Unreal skeletons have it.
    Count_,
};

/// @brief One limb: where it hangs from the body, and its segments.
struct LimbShape
{
    std::string name;
    glm::vec3 attach{0.f, 0.f, 0.f}; ///< In the body's frame, which is the model's.
    float segmentLength = 0.f;
    uint32_t segments = 2; ///< Two or three long segments.
};

/// @brief A whole rig: a root on the ground, a body above it, and limbs that
///        hang straight down from the body.
///
/// The body stands as high as its longest limb, so limbs all of one length
/// reach the ground and a shorter one ends above it.
struct RigShape
{
    std::vector<LimbShape> limbs;
    BoneAxis axis = BoneAxis::PlusY;
};

/// Every limb segment of the ready-made rigs, in metres.
inline constexpr float kSegmentLength = 0.5f;

/// How far either side of the body's centre a limb hangs.
inline constexpr float kHalfWidth = 0.15f;

/// How far in front of and behind the body's centre a four-limbed rig's fore
/// and hind limbs hang.
inline constexpr float kHalfLength = 0.5f;

/// Half the size of the box each joint gets in BuildSkinnedBoxes.
inline constexpr float kBoxHalfExtent = 0.05f;

/// The joints every rig starts with, in the order BuildSkeleton adds them.
inline constexpr int32_t kRootJoint = 0;
inline constexpr int32_t kBodyJoint = 1;

/// The corners of a box and the triangles over them, for BuildSkinnedBoxes.
/// Corner `c` sits on +x when bit 0 is set, +y for bit 1 and +z for bit 2.
inline constexpr uint32_t kBoxCorners = 8;
inline constexpr uint32_t kBoxIndices[] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                           2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};

/// @brief Two limbs side by side under an upright body.
inline RigShape TwoLimbs(uint32_t segments = 2, BoneAxis axis = BoneAxis::PlusY)
{
    RigShape rig;
    rig.axis = axis;
    rig.limbs = {LimbShape{"limb_l", {kHalfWidth, 0.f, 0.f}, kSegmentLength, segments},
                 LimbShape{"limb_r", {-kHalfWidth, 0.f, 0.f}, kSegmentLength, segments}};
    return rig;
}

/// @brief Four limbs under a long body: a fore and a hind pair.
inline RigShape FourLimbs(uint32_t segments = 2, BoneAxis axis = BoneAxis::PlusY)
{
    RigShape rig;
    rig.axis = axis;
    rig.limbs = {LimbShape{"fore_l", {kHalfWidth, 0.f, kHalfLength}, kSegmentLength, segments},
                 LimbShape{"fore_r", {-kHalfWidth, 0.f, kHalfLength}, kSegmentLength, segments},
                 LimbShape{"hind_l", {kHalfWidth, 0.f, -kHalfLength}, kSegmentLength, segments},
                 LimbShape{"hind_r", {-kHalfWidth, 0.f, -kHalfLength}, kSegmentLength, segments}};
    return rig;
}

/// @brief From the body to the ground, along @p limb.
inline float LimbLength(const LimbShape &limb)
{
    return limb.segmentLength * static_cast<float>(limb.segments);
}

/// @brief How many joints BuildSkeleton gives @p rig: the root, the body, and
///        each limb's segments plus its end.
inline uint32_t JointCountOf(const RigShape &rig)
{
    uint32_t joints = static_cast<uint32_t>(kBodyJoint) + 1;
    for (const LimbShape &limb : rig.limbs)
    {
        joints += limb.segments + 1;
    }
    return joints;
}

namespace Detail
{

/// The rotation that turns @p axis to point straight down in the model: half a
/// turn about x for +y, a quarter turn about z for +x.
inline glm::quat DownFrom(BoneAxis axis)
{
    return axis == BoneAxis::PlusY ? glm::angleAxis(glm::pi<float>(), glm::vec3(1.f, 0.f, 0.f))
                                   : glm::angleAxis(-glm::half_pi<float>(), glm::vec3(0.f, 0.f, 1.f));
}

/// @p length along @p axis.
inline glm::vec3 Along(BoneAxis axis, float length)
{
    return axis == BoneAxis::PlusY ? glm::vec3(0.f, length, 0.f) : glm::vec3(length, 0.f, 0.f);
}

inline void AddJoint(Geometry::Skeleton &skeleton, std::string name, int32_t parent,
                     const Geometry::JointTransform &rest)
{
    skeleton.Names.push_back(std::move(name));
    skeleton.Parents.push_back(parent);
    skeleton.RestLocal.push_back(rest);
}

/// Adds @p limb's joints under the body, pointing down along @p axis.
inline void AddLimb(Geometry::Skeleton &skeleton, const LimbShape &limb, BoneAxis axis)
{
    Geometry::JointTransform first;
    first.Translation = limb.attach;
    first.Rotation = DownFrom(axis);
    int32_t parent = kBodyJoint;
    for (uint32_t segment = 0; segment < limb.segments; ++segment)
    {
        Geometry::JointTransform rest;
        rest.Translation = Along(axis, limb.segmentLength);
        AddJoint(skeleton, limb.name + "_" + std::to_string(segment), parent, segment == 0 ? first : rest);
        parent = static_cast<int32_t>(skeleton.Names.size()) - 1;
    }
    Geometry::JointTransform end;
    end.Translation = Along(axis, limb.segmentLength);
    AddJoint(skeleton, limb.name + "_end", parent, end);
}

} // namespace Detail

/// @brief Each joint of @p skeleton at rest, in model space. Parents come
///        before their children, as BuildSkeleton orders them.
inline std::vector<glm::mat4> RestModel(const Geometry::Skeleton &skeleton)
{
    std::vector<glm::mat4> model(skeleton.JointCount(), glm::mat4(1.f));
    for (std::size_t joint = 0; joint < skeleton.JointCount(); ++joint)
    {
        const glm::mat4 local = Geometry::JointMatrix(skeleton.RestLocal[joint]);
        const int32_t parent = skeleton.Parents[joint];
        model[joint] = parent == Geometry::kNoParent ? skeleton.RootTransform * local
                                                     : model[static_cast<std::size_t>(parent)] * local;
    }
    return model;
}

/// @brief The model-space position of joint @p name at rest.
inline glm::vec3 RestPosition(const Geometry::Skeleton &skeleton, std::string_view name)
{
    const int32_t joint = Geometry::FindJoint(skeleton, name);
    return glm::vec3(RestModel(skeleton)[static_cast<std::size_t>(joint)][3]);
}

/// @brief The skeleton @p rig describes, with its inverse bind matrices and a
///        box's bounds around each joint, so it skins and bounds like an
///        imported one.
inline Geometry::Skeleton BuildSkeleton(const RigShape &rig)
{
    float height = 0.f;
    for (const LimbShape &limb : rig.limbs)
    {
        height = glm::max(height, LimbLength(limb));
    }
    Geometry::Skeleton skeleton;
    Detail::AddJoint(skeleton, "root", Geometry::kNoParent, Geometry::JointTransform{});
    Geometry::JointTransform body;
    body.Translation = {0.f, height, 0.f};
    Detail::AddJoint(skeleton, "body", kRootJoint, body);
    for (const LimbShape &limb : rig.limbs)
    {
        Detail::AddLimb(skeleton, limb, rig.axis);
    }

    const glm::vec3 half{kBoxHalfExtent, kBoxHalfExtent, kBoxHalfExtent};
    for (const glm::mat4 &model : RestModel(skeleton))
    {
        skeleton.InverseBind.push_back(glm::inverse(model));
        const glm::vec3 centre{model[3]};
        skeleton.JointBounds.push_back(Geometry::Aabb{.min = centre - half, .max = centre + half});
    }
    return skeleton;
}

/// @brief A box around each joint of @p skeleton, every vertex of it weighted
///        wholly to that joint, so skinning moves each box with its joint.
inline Geometry::MeshData BuildSkinnedBoxes(Geometry::Skeleton skeleton)
{
    Geometry::MeshData mesh;
    const std::vector<glm::mat4> model = RestModel(skeleton);
    for (uint32_t joint = 0; joint < skeleton.JointCount(); ++joint)
    {
        const glm::vec3 centre{model[joint][3]};
        const uint32_t base = joint * kBoxCorners;
        for (uint32_t corner = 0; corner < kBoxCorners; ++corner)
        {
            const glm::vec3 side{(corner & 1u) != 0 ? 1.f : -1.f, (corner & 2u) != 0 ? 1.f : -1.f,
                                 (corner & 4u) != 0 ? 1.f : -1.f};
            Geometry::Vertex vertex;
            vertex.Position = centre + side * kBoxHalfExtent;
            mesh.Vertices.push_back(vertex);
            mesh.Skin.push_back(Geometry::VertexSkin{.Weights = {1.f, 0.f, 0.f, 0.f}, .Joints = {joint, 0u, 0u, 0u}});
        }
        for (const uint32_t index : kBoxIndices)
        {
            mesh.Indices.push_back(base + index);
        }
    }
    mesh.Skeleton = std::move(skeleton);
    return mesh;
}

/// @brief A cycle of @p seconds on @p joint: it turns about @p axis to
///        +@p degrees, back, to -@p degrees and back.
///
/// Keyed at every quarter of the cycle, so a test reading it at a quarter gets
/// exactly 0, +@p degrees, 0 or -@p degrees. One cycle every @p seconds is its
/// speed.
inline std::shared_ptr<const Geometry::AnimationClip> CycleClip(std::string joint, glm::vec3 axis, float degrees,
                                                                float seconds)
{
    constexpr float kQuarter = 0.25f;
    const float angle = glm::radians(degrees);
    Geometry::JointTrack track;
    track.Joint = std::move(joint);
    for (const float sign : {0.f, 1.f, 0.f, -1.f, 0.f})
    {
        track.Rotation.Times.push_back(static_cast<float>(track.Rotation.Times.size()) * kQuarter * seconds);
        track.Rotation.Values.push_back(glm::angleAxis(sign * angle, glm::normalize(axis)));
    }
    Geometry::AnimationClip clip;
    clip.Name = "cycle";
    clip.Tracks = {std::move(track)};
    clip.Duration = seconds;
    return std::make_shared<const Geometry::AnimationClip>(std::move(clip));
}

/// @brief A clip of @p seconds that holds @p joint at @p pose throughout.
inline std::shared_ptr<const Geometry::AnimationClip> HeldClip(std::string joint, const Geometry::JointTransform &pose,
                                                               float seconds)
{
    Geometry::JointTrack track;
    track.Joint = std::move(joint);
    track.Rotation.Times = {0.f};
    track.Rotation.Values = {pose.Rotation};
    track.Translation.Times = {0.f};
    track.Translation.Values = {pose.Translation};
    Geometry::AnimationClip clip;
    clip.Name = "held";
    clip.Tracks = {std::move(track)};
    clip.Duration = seconds;
    return std::make_shared<const Geometry::AnimationClip>(std::move(clip));
}

/// @brief A clip of @p seconds that moves @p joint in a straight line from
///        @p from to @p from + @p distance, both relative to its parent.
inline std::shared_ptr<const Geometry::AnimationClip> MoveClip(std::string joint, glm::vec3 from, glm::vec3 distance,
                                                               float seconds)
{
    Geometry::JointTrack track;
    track.Joint = std::move(joint);
    track.Translation.Times = {0.f, seconds};
    track.Translation.Values = {from, from + distance};
    Geometry::AnimationClip clip;
    clip.Name = "move";
    clip.Tracks = {std::move(track)};
    clip.Duration = seconds;
    return std::make_shared<const Geometry::AnimationClip>(std::move(clip));
}

} // namespace Assisi::Runtime::Testing
